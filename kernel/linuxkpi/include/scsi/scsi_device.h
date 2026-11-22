/* LinuxKPI: struct scsi_device (include/scsi/scsi_device.h), as the
 * midlayer in glue/scsi.c creates them while scanning a host. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/blkdev.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_status.h>

struct Scsi_Host;
struct scsi_cmnd;

enum scsi_device_state {
	SDEV_CREATED = 1, SDEV_RUNNING, SDEV_CANCEL, SDEV_DEL, SDEV_QUIESCE, SDEV_OFFLINE,
	SDEV_TRANSPORT_OFFLINE, SDEV_BLOCK, SDEV_CREATED_BLOCK,
};

struct scsi_device {
	struct Scsi_Host *host;
	struct request_queue *request_queue;
	struct list_head siblings;
	struct list_head same_target_siblings;
	unsigned int id, channel;
	u64 lun;
	unsigned int manufacturer;
	unsigned sector_size;
	void *hostdata;
	unsigned char type;
	char scsi_level;
	unsigned char inquiry_len;
	unsigned char *inquiry;
	const char *vendor, *model, *rev;
	int queue_depth;
	unsigned short max_queue_depth;
	unsigned int eh_timeout;
	unsigned int tagged_supported:1, simple_tags:1, removable:1, changed:1, busy:1, lockable:1, borken:1,
		no_uld_attach:1, select_no_atn:1, no_write_same:1, use_16_for_rw:1, use_10_for_rw:1,
		skip_ms_page_8:1, skip_ms_page_3f:1, use_192_bytes_for_3f:1, no_start_on_add:1, allow_restart:1,
		no_vpd_size:1, manage_start_stop:1, is_visible:1, wce_default_on:1, no_read_disc_info:1,
		no_report_opcodes:1, retry_hwerror:1, last_sector_bug:1, ignore_media_change:1;
	enum scsi_device_state sdev_state;
	struct device sdev_gendev;
	atomic_t iorequest_cnt, iodone_cnt, ioerr_cnt;
	unsigned long sdev_data[];
};
#define to_scsi_device(d) container_of(d, struct scsi_device, sdev_gendev)

struct scsi_target {
	struct scsi_device *starget_sdev_user;
	struct list_head siblings;
	unsigned int channel, id;
	void *hostdata;
	struct device dev;
};
#define scsi_target(sdev) ((struct scsi_target *)NULL)

#define sdev_printk(prefix, sdev, fmt, a...) \
	printk(prefix "scsi %u:%u:%u:%llu: " fmt, (sdev)->host ? lkpi_scsi_host_no((sdev)->host) : 0, \
	       (sdev)->channel, (sdev)->id, (unsigned long long)(sdev)->lun, ##a)
#define scmd_printk(prefix, scmd, fmt, a...) sdev_printk(prefix, (scmd)->device, fmt, ##a)
unsigned int lkpi_scsi_host_no(struct Scsi_Host *shost);

int scsi_change_queue_depth(struct scsi_device *sdev, int depth);
struct scsi_device *scsi_device_lookup(struct Scsi_Host *shost, uint channel, uint id, u64 lun);
struct scsi_device *__scsi_device_lookup(struct Scsi_Host *shost, uint channel, uint id, u64 lun);
int scsi_device_get(struct scsi_device *sdev);
void scsi_device_put(struct scsi_device *sdev);
int scsi_add_device(struct Scsi_Host *host, uint channel, uint target, u64 lun);
void scsi_remove_device(struct scsi_device *sdev);
int scsi_device_set_state(struct scsi_device *sdev, enum scsi_device_state state);
struct scsi_device *__scsi_iterate_devices(struct Scsi_Host *shost, struct scsi_device *prev);
#define shost_for_each_device(sdev, shost) \
	for ((sdev) = __scsi_iterate_devices((shost), NULL); (sdev); (sdev) = __scsi_iterate_devices((shost), (sdev)))
#define __shost_for_each_device(sdev, shost) shost_for_each_device(sdev, shost)
static inline int scsi_device_online(struct scsi_device *sdev) { return sdev->sdev_state != SDEV_OFFLINE && sdev->sdev_state != SDEV_DEL; }
static inline int scsi_device_busy(struct scsi_device *sdev) { return 0; }
static inline void scsi_device_resume(struct scsi_device *sdev) { }
static inline int scsi_device_quiesce(struct scsi_device *sdev) { return 0; }
static inline int scsi_internal_device_block_nowait(struct scsi_device *sdev) { return 0; }
static inline int scsi_internal_device_unblock_nowait(struct scsi_device *sdev, enum scsi_device_state s) { return 0; }
static inline int scsi_rescan_device(struct scsi_device *sdev) { return 0; }
static inline int scsi_is_sas_rphy(const struct device *dev) { return 0; }
static inline int scsi_device_tpgs(struct scsi_device *sdev) { return 0; }
static inline int scsi_vpd_tpg_id(struct scsi_device *sdev, int *rel_id) { return -ENODEV; }
int scsi_execute_cmd(struct scsi_device *sdev, const unsigned char *cmd, blk_opf_t opf, void *buffer,
		     unsigned int bufflen, int timeout, int retries, const void *args);
