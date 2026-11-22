// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: a small SCSI midlayer for Linux host adapter drivers
 * (megaraid_sas, mpt3sas, ...).
 *
 * A host registered with scsi_add_host() + scsi_scan_host() is scanned on
 * a thread of its own (boot doesn't wait): every channel/target, LUN 0,
 * gets sdev_init, INQUIRY and sdev_configure; direct-access devices then get
 * READ CAPACITY and become Syluxium block devices (/dev/sdX, scsi.cpp).
 *
 * Commands: each host has a few slots (tags). A slot holds the
 * [struct request][struct scsi_cmnd][driver's cmd_size] block and a DMA
 * bounce buffer of up to LKPI_SCSI_MAX_XFER bytes (one scatterlist entry,
 * allocated under the device's DMA mask), so callers' buffers can be
 * anywhere. queuecommand() is called in process context; scsi_done()
 * completes the slot. A command that times out goes to the driver's
 * eh_timed_out, then eh_abort_handler, then eh_host_reset_handler.
 */
#include <linux/lkpi_all.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/dma-mapping.h>
#include <linux/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_eh.h>

#define LKPI_SCSI_SLOTS 8
#define LKPI_SCSI_MAX_XFER (128 * 1024)
#define LKPI_SCSI_MAX_HOSTS 16

/* scsi.cpp: the block device side. */
void *lkpi_scsi_disk_add(void *sdev, u64 sectors, u32 sector_size, const char *model, void *hw, int removable);
void lkpi_scsi_disk_remove(void *disk);

struct lkpi_scsi_slot {
	void *chunk;              /* request + scsi_cmnd + priv */
	void *bounce;
	dma_addr_t bounce_dma;
	struct scatterlist sg;
	unsigned char sense[SCSI_SENSE_BUFFERSIZE];
	struct completion done;
	volatile int busy;        /* owned by a caller */
	volatile int inflight;    /* handed to the driver, scsi_done not yet seen */
};

struct lkpi_scsi_host {
	struct Scsi_Host *shost;
	struct lkpi_scsi_slot slots[LKPI_SCSI_SLOTS];
	int nslots;
	spinlock_t lock;
	wait_queue_head_t slot_wait;
	struct blk_mq_hw_ctx hctx;
	volatile int blocked;
};

struct lkpi_sdev_extra {
	struct request_queue q;
	void *disk;
};

static struct Scsi_Host *hosts[LKPI_SCSI_MAX_HOSTS];
static unsigned int next_host_no;
static DEFINE_MUTEX(hosts_lock);

unsigned int lkpi_scsi_host_no(struct Scsi_Host *shost) { return shost->host_no; }

static inline struct lkpi_scsi_host *L(struct Scsi_Host *shost) { return shost->lkpi; }
static inline struct scsi_cmnd *slot_cmd(struct lkpi_scsi_slot *s) { return (struct scsi_cmnd *)((struct request *)s->chunk + 1); }
static inline struct request *slot_rq(struct lkpi_scsi_slot *s) { return s->chunk; }

/* ---- hosts ---- */

struct Scsi_Host *scsi_host_alloc(const struct scsi_host_template *t, int privsize)
{
	struct Scsi_Host *sh = kzalloc(sizeof(*sh) + privsize, GFP_KERNEL);
	struct lkpi_scsi_host *l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!sh || !l) {
		kfree(sh);
		kfree(l);
		return NULL;
	}
	sh->lkpi = l;
	l->shost = sh;
	spin_lock_init(&l->lock);
	init_waitqueue_head(&l->slot_wait);
	INIT_LIST_HEAD(&sh->__devices);
	INIT_LIST_HEAD(&sh->__targets);
	spin_lock_init(&sh->host_lock);
	mutex_init(&sh->scan_mutex);
	init_waitqueue_head(&sh->host_wait);
	sh->hostt = t;
	sh->this_id = t->this_id;
	sh->can_queue = t->can_queue ? t->can_queue : 1;
	sh->cmd_per_lun = t->cmd_per_lun;
	sh->sg_tablesize = t->sg_tablesize ? t->sg_tablesize : 1;
	sh->max_sectors = t->max_sectors ? t->max_sectors : SCSI_DEFAULT_MAX_SECTORS;
	sh->max_segment_size = t->max_segment_size ? t->max_segment_size : 65536;
	sh->dma_boundary = t->dma_boundary ? t->dma_boundary : 0xffffffff;
	sh->max_channel = 0;
	sh->max_id = 8;
	sh->max_lun = 8;
	sh->max_cmd_len = 12;
	sh->nr_hw_queues = 1;
	sh->host_tagset = t->host_tagset;
	sh->shost_state = SHOST_CREATED;
	mutex_lock(&hosts_lock);
	sh->host_no = next_host_no++;
	if (sh->host_no < LKPI_SCSI_MAX_HOSTS)
		hosts[sh->host_no] = sh;
	mutex_unlock(&hosts_lock);
	return sh;
}

int scsi_add_host_with_dma(struct Scsi_Host *sh, struct device *dev, struct device *dma_dev)
{
	struct lkpi_scsi_host *l = L(sh);
	size_t chunk = sizeof(struct request) + sizeof(struct scsi_cmnd) + sh->hostt->cmd_size;
	int i;

	sh->shost_gendev.parent = dev;
	sh->dma_dev = dma_dev ? dma_dev : dev;
	l->nslots = min(sh->can_queue, LKPI_SCSI_SLOTS);
	for (i = 0; i < l->nslots; i++) {
		struct lkpi_scsi_slot *s = &l->slots[i];
		s->chunk = kzalloc(chunk, GFP_KERNEL);
		s->bounce = dma_alloc_coherent(sh->dma_dev, LKPI_SCSI_MAX_XFER, &s->bounce_dma, GFP_KERNEL);
		if (!s->chunk || !s->bounce)
			return -ENOMEM;
		init_completion(&s->done);
		if (sh->hostt->init_cmd_priv)
			sh->hostt->init_cmd_priv(sh, (struct scsi_cmnd *)((struct request *)s->chunk + 1));
	}
	/* One hardware queue is used; the driver's queue maps still get set up. */
	sh->tag_set.nr_hw_queues = sh->nr_hw_queues ? sh->nr_hw_queues : 1;
	sh->tag_set.nr_maps = sh->nr_maps ? sh->nr_maps : 1;
	sh->tag_set.map[HCTX_TYPE_DEFAULT].nr_queues = sh->tag_set.nr_hw_queues;
	sh->tag_set.queue_depth = sh->can_queue;
	sh->tag_set.cmd_size = sh->hostt->cmd_size;
	if (sh->hostt->map_queues)
		sh->hostt->map_queues(sh);
	sh->shost_state = SHOST_RUNNING;
	printk(KERN_INFO "scsi host%u: %s (%d commands, %d at a time here)\n", sh->host_no,
	       sh->hostt->name ? sh->hostt->name : sh->hostt->proc_name, sh->can_queue, l->nslots);
	return 0;
}

struct Scsi_Host *scsi_host_get(struct Scsi_Host *sh) { return sh; }
struct Scsi_Host *scsi_host_lookup(unsigned int hostnum) { return hostnum < LKPI_SCSI_MAX_HOSTS ? hosts[hostnum] : NULL; }
void scsi_block_requests(struct Scsi_Host *sh) { L(sh)->blocked = 1; }
void scsi_unblock_requests(struct Scsi_Host *sh) { L(sh)->blocked = 0; wake_up(&L(sh)->slot_wait); }
int scsi_queue_work(struct Scsi_Host *sh, struct work_struct *work) { return queue_work(system_wq, work); }

int scsi_host_busy(struct Scsi_Host *sh)
{
	int i, n = 0;
	for (i = 0; i < L(sh)->nslots; i++)
		n += L(sh)->slots[i].inflight;
	return n;
}

void scsi_host_busy_iter(struct Scsi_Host *sh, bool (*fn)(struct scsi_cmnd *, void *), void *priv)
{
	int i;
	for (i = 0; i < L(sh)->nslots; i++)
		if (L(sh)->slots[i].inflight && !fn(slot_cmd(&L(sh)->slots[i]), priv))
			break;
}

void scsi_host_complete_all_commands(struct Scsi_Host *sh, enum scsi_host_status status)
{
	int i;
	for (i = 0; i < L(sh)->nslots; i++) {
		struct lkpi_scsi_slot *s = &L(sh)->slots[i];
		if (s->inflight) {
			set_host_byte(slot_cmd(s), status);
			scsi_done(slot_cmd(s));
		}
	}
}

struct scsi_cmnd *scsi_host_find_tag(struct Scsi_Host *sh, int tag)
{
	int t = blk_mq_unique_tag_to_tag(tag);
	if (tag < 0 || t >= L(sh)->nslots || !L(sh)->slots[t].inflight)
		return NULL;
	return slot_cmd(&L(sh)->slots[t]);
}

/* ---- commands ---- */

void scsi_done(struct scsi_cmnd *cmd)
{
	struct lkpi_scsi_slot *s = cmd->lkpi_done_ctx;
	if (!s || !__atomic_exchange_n(&s->inflight, 0, __ATOMIC_ACQ_REL))
		return; /* late completion of a command the error handler finished */
	complete(&s->done);
}
void scsi_done_direct(struct scsi_cmnd *cmd) { scsi_done(cmd); }

int scsi_dma_map(struct scsi_cmnd *cmd) { return scsi_sg_count(cmd); }
void scsi_dma_unmap(struct scsi_cmnd *cmd) { }

size_t lkpi_sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, int to_buffer)
{
	struct scatterlist *sg;
	size_t done = 0;
	int i;
	for_each_sg(sgl, sg, nents, i) {
		size_t n = min_t(size_t, sg->length, buflen - done);
		if (to_buffer)
			memcpy((char *)buf + done, sg_virt(sg), n);
		else
			memcpy(sg_virt(sg), (char *)buf + done, n);
		done += n;
		if (done >= buflen)
			break;
	}
	return done;
}

void scsi_print_command(struct scsi_cmnd *cmd)
{
	char line[3 * 32 + 1];
	int i;
	for (i = 0; i < cmd->cmd_len && i < 32; i++)
		snprintf(line + 3 * i, 4, " %02x", cmd->cmnd[i]);
	line[3 * i] = 0;
	scmd_printk(KERN_INFO, cmd, "CDB:%s\n", line);
}

static struct lkpi_scsi_slot *get_slot(struct lkpi_scsi_host *l, int *tag)
{
	for (;;) {
		int i;
		unsigned long flags;
		spin_lock_irqsave(&l->lock, flags);
		for (i = 0; i < l->nslots; i++)
			if (!l->slots[i].busy) {
				l->slots[i].busy = 1;
				spin_unlock_irqrestore(&l->lock, flags);
				*tag = i;
				return &l->slots[i];
			}
		spin_unlock_irqrestore(&l->lock, flags);
		wait_event_timeout(l->slot_wait, 0, msecs_to_jiffies(2));
	}
}

static void put_slot(struct lkpi_scsi_host *l, struct lkpi_scsi_slot *s)
{
	s->busy = 0;
	wake_up(&l->slot_wait);
}

/* One command. Returns the SCSI result (host byte << 16 | status), or a
 * negative errno if it could not be issued; sense goes to `sense`. */
static int execute(struct scsi_device *sdev, const unsigned char *cdb, int cdb_len, int dir, void *buf,
		   unsigned int len, unsigned char *sense, unsigned int timeout_ms, unsigned int *resid)
{
	struct Scsi_Host *sh = sdev->host;
	struct lkpi_scsi_host *l = L(sh);
	struct lkpi_scsi_slot *s;
	struct scsi_cmnd *cmd;
	struct request *rq;
	int tag, r, tries, result;
	unsigned long left;

	if (len > LKPI_SCSI_MAX_XFER)
		return -EINVAL;
	while (l->blocked || sh->shost_state == SHOST_RECOVERY)
		msleep(10);
	s = get_slot(l, &tag);
	rq = slot_rq(s);
	cmd = slot_cmd(s);
	/* Keep the driver's per-command data (init_cmd_priv set it up). */
	memset(rq, 0, sizeof(*rq));
	memset(cmd, 0, offsetof(struct scsi_cmnd, lkpi_done_ctx));
	rq->tag = tag;
	rq->internal_tag = tag;
	rq->mq_hctx = &l->hctx;
	rq->cmd_flags = dir == DMA_TO_DEVICE ? REQ_OP_WRITE : REQ_OP_READ;
	rq->timeout = msecs_to_jiffies(timeout_ms);
	rq->__data_len = len;
	if (cdb[0] == READ_10 || cdb[0] == WRITE_10)
		rq->__sector = ((u64)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5]) * (sdev->sector_size >> 9);
	else if (cdb[0] == READ_16 || cdb[0] == WRITE_16)
		rq->__sector = get_unaligned_be64(&cdb[2]) * (sdev->sector_size >> 9);
	cmd->device = sdev;
	cmd->cmd_len = cdb_len;
	memcpy(cmd->cmnd, cdb, cdb_len);
	cmd->sc_data_direction = len ? dir : DMA_NONE;
	cmd->sense_buffer = s->sense;
	memset(s->sense, 0, sizeof(s->sense));
	cmd->jiffies_at_alloc = jiffies;
	cmd->allowed = 3;
	cmd->flags = SCMD_TAGGED | SCMD_INITIALIZED;
	cmd->submitter = SUBMITTED_BY_BLOCK_LAYER;
	cmd->budget_token = tag;
	cmd->transfersize = sdev->sector_size;
	cmd->lkpi_done_ctx = s;
	if (len) {
		if (dir == DMA_TO_DEVICE)
			memcpy(s->bounce, buf, len);
		sg_init_table(&s->sg, 1);
		sg_set_buf(&s->sg, s->bounce, len);
		s->sg.dma_address = s->bounce_dma;
		s->sg.dma_length = len;
		cmd->sdb.table.sgl = &s->sg;
		cmd->sdb.table.nents = cmd->sdb.table.orig_nents = 1;
		cmd->sdb.length = len;
	}
	reinit_completion(&s->done);

	for (tries = 0;; tries++) {
		s->inflight = 1;
		r = sh->hostt->queuecommand(sh, cmd);
		if (r == 0)
			break;
		s->inflight = 0;
		if (tries > 1000 || (r != SCSI_MLQUEUE_HOST_BUSY && r != SCSI_MLQUEUE_DEVICE_BUSY &&
				     r != SCSI_MLQUEUE_TARGET_BUSY && r != SCSI_MLQUEUE_EH_RETRY)) {
			put_slot(l, s);
			return -EIO;
		}
		msleep(5);
	}

	for (tries = 0;; tries++) {
		left = wait_for_completion_timeout(&s->done, msecs_to_jiffies(timeout_ms));
		if (left)
			break;
		if (sh->hostt->eh_timed_out && tries < 3 && sh->hostt->eh_timed_out(cmd) == SCSI_EH_RESET_TIMER)
			continue;
		scmd_printk(KERN_WARNING, cmd, "command %02x timed out after %u ms: aborting\n", cdb[0], timeout_ms);
		sh->shost_state = SHOST_RECOVERY;
		r = sh->hostt->eh_abort_handler ? sh->hostt->eh_abort_handler(cmd) : FAILED;
		if (r != SUCCESS && sh->hostt->eh_target_reset_handler)
			r = sh->hostt->eh_target_reset_handler(cmd);
		if (r != SUCCESS && sh->hostt->eh_host_reset_handler)
			r = sh->hostt->eh_host_reset_handler(cmd);
		sh->shost_state = SHOST_RUNNING;
		if (__atomic_exchange_n(&s->inflight, 0, __ATOMIC_ACQ_REL))
			set_host_byte(cmd, DID_TIME_OUT); /* finished by the error handler */
		break;
	}
	result = cmd->result;
	if (len && dir == DMA_FROM_DEVICE && host_byte(result) == DID_OK)
		memcpy(buf, s->bounce, len);
	if (sense)
		memcpy(sense, s->sense, SCSI_SENSE_BUFFERSIZE);
	if (resid)
		*resid = cmd->resid_len;
	put_slot(l, s);
	return result;
}

/* A command with the usual retries (unit attention, becoming ready, busy). */
static int execute_retry(struct scsi_device *sdev, const unsigned char *cdb, int cdb_len, int dir, void *buf,
			 unsigned int len, unsigned int timeout_ms)
{
	unsigned char sense[SCSI_SENSE_BUFFERSIZE];
	struct scsi_sense_hdr sshdr;
	int r, tries;
	for (tries = 0; tries < 5; tries++) {
		r = execute(sdev, cdb, cdb_len, dir, buf, len, sense, timeout_ms, NULL);
		if (r < 0)
			return r;
		if (host_byte(r) == DID_REQUEUE || host_byte(r) == DID_IMM_RETRY || host_byte(r) == DID_BUS_BUSY ||
		    host_byte(r) == DID_SOFT_ERROR)
			continue;
		if (host_byte(r) != DID_OK)
			return r;
		if (status_byte(r) == SAM_STAT_BUSY || status_byte(r) == SAM_STAT_TASK_SET_FULL) {
			msleep(20);
			continue;
		}
		if (status_byte(r) == SAM_STAT_CHECK_CONDITION && scsi_normalize_sense(sense, sizeof(sense), &sshdr)) {
			if (sshdr.sense_key == UNIT_ATTENTION)
				continue;
			if (sshdr.sense_key == NOT_READY && (sshdr.sense_code >> 8) == 0x04) {
				msleep(500);
				continue;
			}
		}
		return r;
	}
	return r;
}

int scsi_execute_cmd(struct scsi_device *sdev, const unsigned char *cmd, blk_opf_t opf, void *buffer,
		     unsigned int bufflen, int timeout, int retries, const void *args)
{
	int dir = !bufflen ? DMA_NONE : (opf & REQ_OP_MASK) == REQ_OP_DRV_OUT ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	return execute_retry(sdev, cmd, COMMAND_SIZE(cmd[0]), dir, buffer, bufflen, jiffies_to_msecs(timeout));
}

/* Read or write `count` sectors (scsi.cpp; count * sector size <= max). */
int lkpi_scsi_rw(void *sdevp, u64 lba, u32 count, void *buf, int write)
{
	struct scsi_device *sdev = sdevp;
	unsigned char cdb[16] = { 0 };
	int len, r;
	if (sdev->sdev_state != SDEV_RUNNING)
		return -ENODEV;
	if (lba + count > 0xffffffffull || sdev->use_16_for_rw) {
		cdb[0] = write ? WRITE_16 : READ_16;
		put_unaligned_be64(lba, &cdb[2]);
		put_unaligned_be32(count, &cdb[10]);
		len = 16;
	} else {
		cdb[0] = write ? WRITE_10 : READ_10;
		put_unaligned_be32((u32)lba, &cdb[2]);
		put_unaligned_be16(count, &cdb[7]);
		len = 10;
	}
	r = execute_retry(sdev, cdb, len, write ? DMA_TO_DEVICE : DMA_FROM_DEVICE, buf, count * sdev->sector_size, 30000);
	if (r) {
		sdev_printk(KERN_WARNING, sdev, "%s of %u sectors at %llu failed (result %#x)\n", write ? "write" : "read",
			    count, (unsigned long long)lba, r);
		return -EIO;
	}
	return 0;
}

int lkpi_scsi_flush(void *sdevp)
{
	unsigned char cdb[10] = { SYNCHRONIZE_CACHE };
	return execute_retry(sdevp, cdb, 10, DMA_NONE, NULL, 0, 60000) ? -EIO : 0;
}

/* Largest transfer in sectors for scsi.cpp. */
u32 lkpi_scsi_max_sectors(void *sdevp)
{
	struct scsi_device *sdev = sdevp;
	u32 n = LKPI_SCSI_MAX_XFER / sdev->sector_size;
	u32 hw = sdev->host->max_sectors * 512 / sdev->sector_size;
	return hw && hw < n ? hw : n;
}

/* ---- devices ---- */

static struct scsi_device *sdev_alloc(struct Scsi_Host *sh, uint channel, uint id, u64 lun)
{
	struct scsi_device *sdev = kzalloc(sizeof(*sdev) + sizeof(struct lkpi_sdev_extra), GFP_KERNEL);
	struct lkpi_sdev_extra *x;
	if (!sdev)
		return NULL;
	x = (void *)sdev->sdev_data;
	sdev->host = sh;
	sdev->channel = channel;
	sdev->id = id;
	sdev->lun = lun;
	sdev->request_queue = &x->q;
	x->q.rq_timeout = 30 * HZ;
	sdev->sector_size = 512;
	sdev->queue_depth = sh->cmd_per_lun ? sh->cmd_per_lun : 1;
	sdev->sdev_state = SDEV_CREATED;
	sdev->sdev_gendev.parent = &sh->shost_gendev;
	snprintf(sdev->sdev_gendev.lkpi_name, sizeof(sdev->sdev_gendev.lkpi_name), "%u:%u:%u:%llu", sh->host_no,
		 channel, id, (unsigned long long)lun);
	INIT_LIST_HEAD(&sdev->siblings);
	return sdev;
}

static void sdev_free(struct scsi_device *sdev)
{
	kfree(sdev->inquiry);
	kfree(sdev);
}

static void strtrim(char *s)
{
	int n = strlen(s);
	while (n && s[n - 1] == ' ')
		s[--n] = 0;
}

/* Probe one LUN; 0 when a device is there (and added). */
static int scan_one(struct Scsi_Host *sh, uint channel, uint id, u64 lun)
{
	struct scsi_device *sdev;
	struct lkpi_sdev_extra *x;
	unsigned char *inq, cdb[16];
	struct queue_limits lim;
	int r;

	if (__scsi_device_lookup(sh, channel, id, lun))
		return 0;
	sdev = sdev_alloc(sh, channel, id, lun);
	if (!sdev)
		return -ENOMEM;
	x = (void *)sdev->sdev_data;
	if (sh->hostt->sdev_init && sh->hostt->sdev_init(sdev)) {
		sdev_free(sdev);
		return -ENXIO;
	}
	sdev->sdev_state = SDEV_RUNNING; /* commands may be issued now */
	inq = kzalloc(96, GFP_KERNEL);
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = INQUIRY;
	cdb[4] = 96;
	r = inq ? execute_retry(sdev, cdb, 6, DMA_FROM_DEVICE, inq, 96, 10000) : -ENOMEM;
	if (r || (inq[0] >> 5) != 0 /* peripheral qualifier: no device here */) {
		if (sh->hostt->sdev_destroy)
			sh->hostt->sdev_destroy(sdev);
		kfree(inq);
		sdev_free(sdev);
		return -ENODEV;
	}
	sdev->inquiry = inq;
	sdev->inquiry_len = 96;
	sdev->type = inq[0] & 0x1f;
	sdev->scsi_level = (inq[2] & 0x07) + 1;
	sdev->removable = (inq[1] & 0x80) != 0;
	sdev->tagged_supported = (inq[7] & 0x02) != 0;
	sdev->vendor = (char *)inq + 8;
	sdev->model = (char *)inq + 16;
	sdev->rev = (char *)inq + 32;
	memset(&lim, 0, sizeof(lim));
	lim.max_hw_sectors = lim.max_sectors = sh->max_sectors;
	lim.max_segments = sh->sg_tablesize;
	lim.max_segment_size = sh->max_segment_size;
	lim.logical_block_size = 512;
	if (sh->hostt->sdev_configure && sh->hostt->sdev_configure(sdev, &lim)) {
		if (sh->hostt->sdev_destroy)
			sh->hostt->sdev_destroy(sdev);
		sdev_free(sdev);
		return -ENODEV;
	}
	sdev->request_queue->limits = lim;
	mutex_lock(&sh->scan_mutex);
	list_add_tail(&sdev->siblings, &sh->__devices);
	mutex_unlock(&sh->scan_mutex);

	if (sdev->type == TYPE_DISK || sdev->type == TYPE_RBC) {
		unsigned char cap[32];
		u64 sectors = 0;
		u32 bs = 512;
		char model[40];
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = READ_CAPACITY;
		if (!execute_retry(sdev, cdb, 10, DMA_FROM_DEVICE, cap, 8, 10000)) {
			sectors = (u64)get_unaligned_be32(cap) + 1;
			bs = get_unaligned_be32(cap + 4);
		}
		if (sectors == 0x100000000ull || !sectors) {
			memset(cdb, 0, sizeof(cdb));
			cdb[0] = SERVICE_ACTION_IN_16;
			cdb[1] = SAI_READ_CAPACITY_16;
			cdb[13] = 32;
			if (!execute_retry(sdev, cdb, 16, DMA_FROM_DEVICE, cap, 32, 10000)) {
				sectors = get_unaligned_be64(cap) + 1;
				bs = get_unaligned_be32(cap + 8);
				sdev->use_16_for_rw = 1;
			}
		}
		if (bs < 512 || bs > 4096 || (bs & (bs - 1)))
			bs = 512;
		sdev->sector_size = bs;
		snprintf(model, sizeof(model), "%.8s %.16s", sdev->vendor, sdev->model);
		strtrim(model);
		sdev_printk(KERN_INFO, sdev, "Direct-Access %s, %llu sectors of %u bytes (%llu MB)\n", model,
			    (unsigned long long)sectors, bs, (unsigned long long)(sectors * bs >> 20));
		if (sectors)
			x->disk = lkpi_scsi_disk_add(sdev, sectors, bs, model, sh->shost_gendev.parent ?
						     sh->shost_gendev.parent->lkpi_registry : NULL, sdev->removable);
	} else {
		sdev_printk(KERN_INFO, sdev, "type %#x %.8s %.16s (no driver for this kind of device)\n", sdev->type,
			    sdev->vendor, sdev->model);
	}
	return 0;
}

static int scan_thread(void *arg)
{
	struct Scsi_Host *sh = arg;
	uint ch, id, found = 0;
	unsigned long start = jiffies;
	uint max_id = min_t(uint, sh->max_id, 256);
	if (sh->hostt->scan_start)
		sh->hostt->scan_start(sh);
	if (sh->hostt->scan_finished)
		while (!sh->hostt->scan_finished(sh, jiffies - start))
			msleep(10);
	for (ch = 0; ch <= sh->max_channel && ch < 8; ch++)
		for (id = 0; id < max_id; id++)
			if ((int)id != sh->this_id && scan_one(sh, ch, id, 0) == 0)
				found++;
	printk(KERN_INFO "scsi host%u: scan done in %u ms: %u device(s)\n", sh->host_no,
	       jiffies_to_msecs(jiffies - start), found);
	return 0;
}

void scsi_scan_host(struct Scsi_Host *sh)
{
	kthread_run(scan_thread, sh, "scsi_scan");
}

void scsi_scan_target(struct device *parent, unsigned int channel, unsigned int id, u64 lun, int rescan)
{
	struct Scsi_Host *sh = dev_to_shost(parent);
	scan_one(sh, channel, id, lun == SCAN_WILD_CARD ? 0 : lun);
}

int scsi_add_device(struct Scsi_Host *sh, uint channel, uint target, u64 lun)
{
	return scan_one(sh, channel, target, lun);
}

void scsi_remove_device(struct scsi_device *sdev)
{
	struct Scsi_Host *sh = sdev->host;
	struct lkpi_sdev_extra *x = (void *)sdev->sdev_data;
	if (sdev->sdev_state == SDEV_DEL)
		return;
	sdev->sdev_state = SDEV_DEL;
	if (x->disk)
		lkpi_scsi_disk_remove(x->disk);
	mutex_lock(&sh->scan_mutex);
	list_del(&sdev->siblings);
	mutex_unlock(&sh->scan_mutex);
	if (sh->hostt->sdev_destroy)
		sh->hostt->sdev_destroy(sdev);
	/* The scsi_device stays allocated: a block device may still point at it. */
}

void scsi_remove_host(struct Scsi_Host *sh)
{
	struct scsi_device *sdev, *n;
	sh->shost_state = SHOST_DEL;
	list_for_each_entry_safe(sdev, n, &sh->__devices, siblings)
		scsi_remove_device(sdev);
}

void scsi_host_put(struct Scsi_Host *sh) { }

struct scsi_device *__scsi_device_lookup(struct Scsi_Host *sh, uint channel, uint id, u64 lun)
{
	struct scsi_device *sdev;
	list_for_each_entry(sdev, &sh->__devices, siblings)
		if (sdev->channel == channel && sdev->id == id && sdev->lun == lun)
			return sdev;
	return NULL;
}

struct scsi_device *scsi_device_lookup(struct Scsi_Host *sh, uint channel, uint id, u64 lun)
{
	struct scsi_device *sdev;
	mutex_lock(&sh->scan_mutex);
	sdev = __scsi_device_lookup(sh, channel, id, lun);
	mutex_unlock(&sh->scan_mutex);
	return sdev;
}

struct scsi_device *__scsi_iterate_devices(struct Scsi_Host *sh, struct scsi_device *prev)
{
	struct list_head *next = prev ? prev->siblings.next : sh->__devices.next;
	return next == &sh->__devices ? NULL : list_entry(next, struct scsi_device, siblings);
}

int scsi_device_get(struct scsi_device *sdev) { return sdev->sdev_state == SDEV_DEL ? -ENXIO : 0; }
void scsi_device_put(struct scsi_device *sdev) { }
int scsi_device_set_state(struct scsi_device *sdev, enum scsi_device_state state)
{
	sdev->sdev_state = state;
	return 0;
}

int scsi_change_queue_depth(struct scsi_device *sdev, int depth)
{
	if (depth > 0)
		sdev->queue_depth = depth;
	return sdev->queue_depth;
}
