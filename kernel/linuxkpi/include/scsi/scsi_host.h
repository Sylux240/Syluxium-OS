/* LinuxKPI: SCSI host adapters (include/scsi/scsi_host.h). The midlayer
 * behind it is glue/scsi.c: hosts scan their targets in the background
 * and every disk found becomes a Syluxium block device (/dev/sdX). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/seq_file.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>

struct scsi_cmnd;
struct scsi_target;
struct scsi_transport_template;
struct gendisk;
struct attribute_group;

#define SG_NONE 0
#define SG_ALL SG_CHUNK_SIZE
#ifndef SG_CHUNK_SIZE
#define SG_CHUNK_SIZE 128
#endif
#define MODE_UNKNOWN 0x00
#define MODE_INITIATOR 0x01
#define MODE_TARGET 0x02
#define DISABLE_CLUSTERING (-1)
#define ENABLE_CLUSTERING 0
#define SCSI_DEFAULT_HOST_BLOCKED 7
#define SCSI_MAX_SG_SEGMENTS SG_CHUNK_SIZE
#define SCSI_MAX_SG_CHAIN_SEGMENTS 2048
#define SCSI_DEFAULT_MAX_SECTORS 1024

enum scsi_timeout_action { SCSI_EH_DONE, SCSI_EH_RESET_TIMER, SCSI_EH_NOT_HANDLED };

struct scsi_host_template {
	unsigned int cmd_size;
	enum scsi_qc_status (*queuecommand)(struct Scsi_Host *, struct scsi_cmnd *);
	enum scsi_qc_status (*queue_reserved_command)(struct Scsi_Host *, struct scsi_cmnd *);
	void (*commit_rqs)(struct Scsi_Host *, u16);
	struct module *module;
	const char *name;
	const char *(*info)(struct Scsi_Host *);
	int (*ioctl)(struct scsi_device *dev, unsigned int cmd, void __user *arg);
	int (*compat_ioctl)(struct scsi_device *dev, unsigned int cmd, void __user *arg);
	int (*init_cmd_priv)(struct Scsi_Host *shost, struct scsi_cmnd *cmd);
	int (*exit_cmd_priv)(struct Scsi_Host *shost, struct scsi_cmnd *cmd);
	int (*eh_abort_handler)(struct scsi_cmnd *);
	int (*eh_device_reset_handler)(struct scsi_cmnd *);
	int (*eh_target_reset_handler)(struct scsi_cmnd *);
	int (*eh_bus_reset_handler)(struct scsi_cmnd *);
	int (*eh_host_reset_handler)(struct scsi_cmnd *);
	int (*sdev_init)(struct scsi_device *);
	int (*sdev_configure)(struct scsi_device *, struct queue_limits *lim);
	void (*sdev_destroy)(struct scsi_device *);
	int (*target_alloc)(struct scsi_target *);
	void (*target_destroy)(struct scsi_target *);
	int (*scan_finished)(struct Scsi_Host *, unsigned long);
	void (*scan_start)(struct Scsi_Host *);
	int (*change_queue_depth)(struct scsi_device *, int);
	void (*map_queues)(struct Scsi_Host *shost);
	int (*mq_poll)(struct Scsi_Host *shost, unsigned int queue_num);
	bool (*dma_need_drain)(struct request *rq);
	int (*bios_param)(struct scsi_device *, struct gendisk *, sector_t, int[]);
	void (*unlock_native_capacity)(struct scsi_device *);
	int (*show_info)(struct seq_file *, struct Scsi_Host *);
	int (*write_info)(struct Scsi_Host *, char *, int);
	enum scsi_timeout_action (*eh_timed_out)(struct scsi_cmnd *);
	bool (*eh_should_retry_cmd)(struct scsi_cmnd *scmd);
	int (*host_reset)(struct Scsi_Host *shost, int reset_type);
#define SCSI_ADAPTER_RESET 1
#define SCSI_FIRMWARE_RESET 2
	const char *proc_name;
	int can_queue;
	int nr_reserved_cmds;
	int this_id;
	unsigned short sg_tablesize;
	unsigned short sg_prot_tablesize;
	unsigned int max_sectors;
	unsigned int max_segment_size;
	unsigned int dma_alignment;
	unsigned long dma_boundary;
	unsigned long virt_boundary_mask;
	short cmd_per_lun;
	bool tag_alloc_policy_rr : 1;
	unsigned track_queue_depth:1;
	unsigned supported_mode:2;
	unsigned emulated:1;
	unsigned skip_settle_delay:1;
	unsigned no_write_same:1;
	unsigned host_tagset:1;
	unsigned queuecommand_may_block:1;
	unsigned int max_host_blocked;
	const struct attribute_group **shost_groups;
	const struct attribute_group **sdev_groups;
	u64 vendor_id;
};

enum scsi_host_state { SHOST_CREATED = 1, SHOST_RUNNING, SHOST_CANCEL, SHOST_DEL, SHOST_RECOVERY,
		       SHOST_CANCEL_RECOVERY, SHOST_DEL_RECOVERY };

struct Scsi_Host {
	struct list_head __devices;
	struct list_head __targets;
	spinlock_t host_lock;
	struct mutex scan_mutex;
	wait_queue_head_t host_wait;
	const struct scsi_host_template *hostt;
	struct scsi_transport_template *transportt;
	struct blk_mq_tag_set tag_set;
	atomic_t host_blocked;
	unsigned int host_failed;
	unsigned int host_eh_scheduled;
	unsigned int host_no;
	int eh_deadline;
	unsigned long last_reset;
	unsigned int max_channel;
	unsigned int max_id;
	u64 max_lun;
	unsigned int unique_id;
	unsigned short max_cmd_len;
	int this_id;
	int can_queue;
	unsigned int nr_reserved_cmds;
	short cmd_per_lun;
	unsigned short sg_tablesize;
	unsigned short sg_prot_tablesize;
	unsigned int max_sectors;
	unsigned int opt_sectors;
	unsigned int max_segment_size;
	unsigned int dma_alignment;
	unsigned long dma_boundary;
	unsigned long virt_boundary_mask;
	unsigned nr_hw_queues;
	unsigned nr_maps;
	bool async_scan;
	bool eh_noresume;
	unsigned active_mode:2;
	unsigned host_self_blocked:1;
	unsigned reverse_ordering:1;
	unsigned tmf_in_progress:1;
	unsigned no_write_same:1;
	unsigned host_tagset:1;
	unsigned queuecommand_may_block:1;
	unsigned short_inquiry:1;
	unsigned no_scsi2_lun_in_cdb:1;
	struct workqueue_struct *work_q;
	struct workqueue_struct *tmf_work_q;
	unsigned int max_host_blocked;
	unsigned int prot_capabilities;
	unsigned char prot_guard_type;
	unsigned long base;
	unsigned long io_port;
	unsigned char n_io_port;
	unsigned char dma_channel;
	unsigned int irq;
	enum scsi_host_state shost_state;
	struct device shost_gendev, shost_dev;
	void *shost_data;
	struct device *dma_dev;
	/* LinuxKPI midlayer state (glue/scsi.c) */
	void *lkpi;
	unsigned long hostdata[] __aligned(sizeof(unsigned long));
};

#define class_to_shost(d) container_of(d, struct Scsi_Host, shost_dev)
#define dev_to_shost(d) container_of(d, struct Scsi_Host, shost_gendev)
#define shost_printk(prefix, shost, fmt, a...) printk(prefix "scsi host%u: " fmt, (shost)->host_no, ##a)

static inline void *shost_priv(struct Scsi_Host *shost) { return (void *)shost->hostdata; }

struct Scsi_Host *scsi_host_alloc(const struct scsi_host_template *t, int privsize);
int scsi_add_host_with_dma(struct Scsi_Host *shost, struct device *dev, struct device *dma_dev);
static inline int scsi_add_host(struct Scsi_Host *host, struct device *dev) { return scsi_add_host_with_dma(host, dev, dev); }
void scsi_scan_host(struct Scsi_Host *shost);
void scsi_remove_host(struct Scsi_Host *shost);
void scsi_host_put(struct Scsi_Host *shost);
struct Scsi_Host *scsi_host_get(struct Scsi_Host *shost);
struct Scsi_Host *scsi_host_lookup(unsigned int hostnum);
void scsi_block_requests(struct Scsi_Host *shost);
void scsi_unblock_requests(struct Scsi_Host *shost);
int scsi_host_busy(struct Scsi_Host *shost);
void scsi_host_complete_all_commands(struct Scsi_Host *shost, enum scsi_host_status status);
void scsi_host_busy_iter(struct Scsi_Host *shost, bool (*fn)(struct scsi_cmnd *, void *), void *priv);
static inline struct device *scsi_get_device(struct Scsi_Host *shost) { return shost->shost_gendev.parent; }
static inline int scsi_host_in_recovery(struct Scsi_Host *shost) { return shost->shost_state == SHOST_RECOVERY; }
static inline enum scsi_host_state scsi_get_host_state(struct Scsi_Host *shost) { return shost->shost_state; }
static inline int scsi_host_scan_allowed(struct Scsi_Host *shost) { return shost->shost_state == SHOST_RUNNING; }
static inline void scsi_host_set_prot(struct Scsi_Host *shost, unsigned int mask) { shost->prot_capabilities = mask; }
static inline void scsi_host_set_guard(struct Scsi_Host *shost, unsigned char type) { shost->prot_guard_type = type; }
#define SHOST_DIF_TYPE1_PROTECTION (1 << 0)
#define SHOST_DIX_TYPE0_PROTECTION (1 << 3)
#define SHOST_DIX_GUARD_CRC (1 << 0)
#define SHOST_DIX_GUARD_IP (1 << 1)
void scsi_scan_target(struct device *parent, unsigned int channel, unsigned int id, u64 lun, int rescan);
#define SCSI_SCAN_INITIAL 0
#define SCSI_SCAN_RESCAN 1
#define SCSI_SCAN_MANUAL 2
static inline void scsi_flush_work(struct Scsi_Host *shost) { }
int scsi_queue_work(struct Scsi_Host *shost, struct work_struct *work);
