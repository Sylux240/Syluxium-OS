/* LinuxKPI: struct scsi_cmnd (include/scsi/scsi_cmnd.h). The midlayer
 * (glue/scsi.c) allocates [struct request][struct scsi_cmnd][driver's
 * cmd_size bytes] in one piece, so scsi_cmd_to_rq() and scsi_cmd_priv()
 * are pointer arithmetic as in Linux. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/blkdev.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <scsi/scsi_device.h>

struct Scsi_Host;

#define SCSI_SENSE_BUFFERSIZE 96

struct scsi_data_buffer {
	struct sg_table table;
	unsigned length;
};

enum scsi_cmnd_submitter { SUBMITTED_BY_BLOCK_LAYER = 0, SUBMITTED_BY_SCSI_ERROR_HANDLER = 1, SUBMITTED_BY_SCSI_RESET_IOCTL = 2 };

#define SCMD_TAGGED (1 << 0)
#define SCMD_INITIALIZED (1 << 1)
#define SCMD_LAST (1 << 2)
#define SCMD_FAIL_IF_RECOVERING (1 << 4)
#define SCMD_STATE_COMPLETE 0
#define SCMD_STATE_INFLIGHT 1

struct scsi_cmnd {
	struct scsi_device *device;
	struct list_head eh_entry;
	struct delayed_work abort_work;
	struct rcu_head rcu;
	int eh_eflags;
	int budget_token;
	unsigned long jiffies_at_alloc;
	int retries;
	int allowed;
	unsigned char prot_op;
	unsigned char prot_type;
	unsigned char prot_flags;
	enum scsi_cmnd_submitter submitter;
	unsigned short cmd_len;
	enum dma_data_direction sc_data_direction;
	unsigned char cmnd[32];
	struct scsi_data_buffer sdb;
	struct scsi_data_buffer *prot_sdb;
	unsigned underflow;
	unsigned transfersize;
	unsigned resid_len;
	unsigned sense_len;
	unsigned char *sense_buffer;
	int flags;
	unsigned long state;
	unsigned int extra_len;
	unsigned char *host_scribble;
	int result;
	/* LinuxKPI: completion of this command (glue/scsi.c) */
	void *lkpi_done_ctx;
};

static inline struct request *scsi_cmd_to_rq(struct scsi_cmnd *scmd) { return blk_mq_rq_from_pdu(scmd); }
static inline void *scsi_cmd_priv(struct scsi_cmnd *cmd) { return cmd + 1; }

void scsi_done(struct scsi_cmnd *cmd);
void scsi_done_direct(struct scsi_cmnd *cmd);

int scsi_dma_map(struct scsi_cmnd *cmd);
void scsi_dma_unmap(struct scsi_cmnd *cmd);
static inline unsigned scsi_sg_count(struct scsi_cmnd *cmd) { return cmd->sdb.table.nents; }
static inline struct scatterlist *scsi_sglist(struct scsi_cmnd *cmd) { return cmd->sdb.table.sgl; }
static inline unsigned scsi_bufflen(struct scsi_cmnd *cmd) { return cmd->sdb.length; }
static inline void scsi_set_resid(struct scsi_cmnd *cmd, unsigned int resid) { cmd->resid_len = resid; }
static inline unsigned int scsi_get_resid(struct scsi_cmnd *cmd) { return cmd->resid_len; }
#define scsi_for_each_sg(cmd, sg, nseg, __i) for_each_sg(scsi_sglist(cmd), sg, nseg, __i)
size_t lkpi_sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, int to_buffer);
static inline int scsi_sg_copy_from_buffer(struct scsi_cmnd *cmd, const void *buf, int buflen)
{
	return lkpi_sg_copy_buffer(scsi_sglist(cmd), scsi_sg_count(cmd), (void *)buf, buflen, 0);
}
static inline int scsi_sg_copy_to_buffer(struct scsi_cmnd *cmd, void *buf, int buflen)
{
	return lkpi_sg_copy_buffer(scsi_sglist(cmd), scsi_sg_count(cmd), buf, buflen, 1);
}
static inline sector_t scsi_get_sector(struct scsi_cmnd *scmd) { return blk_rq_pos(scsi_cmd_to_rq(scmd)); }
static inline sector_t scsi_get_lba(struct scsi_cmnd *scmd)
{
	unsigned int shift = ilog2(scmd->device->sector_size ? scmd->device->sector_size : 512) - SECTOR_SHIFT;
	return blk_rq_pos(scsi_cmd_to_rq(scmd)) >> shift;
}
static inline unsigned int scsi_logical_block_count(struct scsi_cmnd *scmd)
{
	unsigned int shift = ilog2(scmd->device->sector_size ? scmd->device->sector_size : 512);
	return blk_rq_bytes(scsi_cmd_to_rq(scmd)) >> shift;
}

/* No T10 protection information. */
enum scsi_prot_operations { SCSI_PROT_NORMAL = 0, SCSI_PROT_READ_INSERT, SCSI_PROT_WRITE_STRIP, SCSI_PROT_READ_STRIP,
			    SCSI_PROT_WRITE_INSERT, SCSI_PROT_READ_PASS, SCSI_PROT_WRITE_PASS };
enum scsi_prot_target_type { T10_PI_TYPE0_PROTECTION = 0, T10_PI_TYPE1_PROTECTION, T10_PI_TYPE2_PROTECTION, T10_PI_TYPE3_PROTECTION };
#define SCSI_PROT_TRANSFER_PI (1 << 0)
#define SCSI_PROT_GUARD_CHECK (1 << 1)
#define SCSI_PROT_REF_CHECK (1 << 2)
#define SCSI_PROT_REF_INCREMENT (1 << 3)
#define SCSI_PROT_IP_CHECKSUM (1 << 4)
static inline unsigned char scsi_get_prot_op(struct scsi_cmnd *scmd) { return scmd->prot_op; }
static inline unsigned char scsi_get_prot_type(struct scsi_cmnd *scmd) { return scmd->prot_type; }
static inline unsigned scsi_prot_sg_count(struct scsi_cmnd *cmd) { return 0; }
static inline struct scatterlist *scsi_prot_sglist(struct scsi_cmnd *cmd) { return NULL; }
static inline u32 scsi_prot_ref_tag(struct scsi_cmnd *scmd) { return (u32)scsi_get_lba(scmd); }
static inline unsigned int scsi_prot_interval(struct scsi_cmnd *scmd) { return scmd->device->sector_size; }

static inline void set_status_byte(struct scsi_cmnd *cmd, char status) { cmd->result = (cmd->result & 0xffffff00) | status; }
static inline u8 get_status_byte(struct scsi_cmnd *cmd) { return cmd->result & 0xff; }
static inline void set_host_byte(struct scsi_cmnd *cmd, char status) { cmd->result = (cmd->result & 0xff00ffff) | (status << 16); }
static inline u8 get_host_byte(struct scsi_cmnd *cmd) { return (cmd->result >> 16) & 0xff; }
static inline unsigned scsi_transfer_length(struct scsi_cmnd *scmd) { return scsi_bufflen(scmd); }
static inline bool scsi_cmd_allowed_retry(struct scsi_cmnd *scmd) { return scmd->retries < scmd->allowed; }

/* Debug printing (scsi_dbg.h). */
void scsi_print_command(struct scsi_cmnd *cmd);
static inline void scsi_print_sense(const struct scsi_cmnd *cmd) { }
static inline void scsi_print_result(const struct scsi_cmnd *cmd, const char *msg, int disposition) { }
