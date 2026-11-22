/* LinuxKPI: the block layer as SCSI/storage drivers see it.
 *
 * There is no Linux block layer here: Syluxium's own (kernel/block) issues
 * whole-sector reads and writes, and the LinuxKPI SCSI midlayer
 * (glue/scsi.c) turns each into one SCSI command with a struct request
 * in front of it, so drivers' tag and queue helpers keep working. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/scatterlist.h>

#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT 9
#endif
#ifndef SECTOR_SIZE
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#endif
typedef u64 sector_t;
typedef u64 blkcnt_t;

static inline u32 lkpi_sector_div(sector_t *s, u32 div)
{
	u32 r = *s % div;
	*s /= div;
	return r;
}
#define sector_div(n, base) lkpi_sector_div(&(n), (base))

enum req_op { REQ_OP_READ = 0, REQ_OP_WRITE = 1, REQ_OP_FLUSH = 2, REQ_OP_DRV_IN = 34, REQ_OP_DRV_OUT = 35 };
typedef unsigned int blk_opf_t;
typedef u8 blk_status_t;
#define BLK_STS_OK 0
#define BLK_STS_IOERR 10
#define REQ_OP_MASK 0xff
#define REQ_FAILFAST_DEV (1u << 8)
#define REQ_POLLED (1u << 21)

struct blk_mq_hw_ctx {
	unsigned int queue_num;
	void *driver_data;
	unsigned int type;
};

struct request_queue;
struct gendisk;

/* One per in-flight command, directly in front of the scsi_cmnd. */
struct request {
	struct request_queue *q;
	struct blk_mq_hw_ctx *mq_hctx;
	blk_opf_t cmd_flags;
	int tag;
	int internal_tag;
	unsigned int timeout;
	sector_t __sector;
	unsigned int __data_len;
	unsigned long deadline;
	void *end_io_data;
};
static inline enum req_op req_op(const struct request *rq) { return (enum req_op)(rq->cmd_flags & REQ_OP_MASK); }
static inline sector_t blk_rq_pos(const struct request *rq) { return rq->__sector; }
static inline unsigned int blk_rq_bytes(const struct request *rq) { return rq->__data_len; }
static inline unsigned int blk_rq_sectors(const struct request *rq) { return rq->__data_len >> SECTOR_SHIFT; }
static inline bool blk_rq_is_passthrough(struct request *rq) { return req_op(rq) >= REQ_OP_DRV_IN; }
static inline void *blk_mq_rq_to_pdu(struct request *rq) { return rq + 1; }
static inline struct request *blk_mq_rq_from_pdu(void *pdu) { return (struct request *)pdu - 1; }

#define BLK_MQ_UNIQUE_TAG_BITS 16
#define BLK_MQ_UNIQUE_TAG_MASK ((1 << BLK_MQ_UNIQUE_TAG_BITS) - 1)
static inline u32 blk_mq_unique_tag(struct request *rq)
{
	return ((rq->mq_hctx ? rq->mq_hctx->queue_num : 0) << BLK_MQ_UNIQUE_TAG_BITS) | (rq->tag & BLK_MQ_UNIQUE_TAG_MASK);
}
static inline u16 blk_mq_unique_tag_to_hwq(u32 unique_tag) { return unique_tag >> BLK_MQ_UNIQUE_TAG_BITS; }
static inline u16 blk_mq_unique_tag_to_tag(u32 unique_tag) { return unique_tag & BLK_MQ_UNIQUE_TAG_MASK; }

/* Queue maps: one hardware queue here (the midlayer issues on queue 0). */
enum hctx_type { HCTX_TYPE_DEFAULT, HCTX_TYPE_READ, HCTX_TYPE_POLL, HCTX_MAX_TYPES };
struct blk_mq_queue_map {
	unsigned int *mq_map;
	unsigned int nr_queues;
	unsigned int queue_offset;
};
struct blk_mq_tag_set {
	struct blk_mq_queue_map map[HCTX_MAX_TYPES];
	unsigned int nr_maps;
	unsigned int nr_hw_queues;
	unsigned int queue_depth;
	unsigned int cmd_size;
	void *driver_data;
};
static inline void blk_mq_map_queues(struct blk_mq_queue_map *qmap) { qmap->nr_queues = qmap->nr_queues ? qmap->nr_queues : 1; }
static inline void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap, struct device *dev, unsigned int offset)
{
	blk_mq_map_queues(qmap);
}
static inline unsigned int blk_mq_num_online_queues(unsigned int max_queues) { return 1; }
static inline unsigned int blk_mq_num_possible_queues(unsigned int max_queues) { return 1; }

struct queue_limits {
	unsigned int max_hw_sectors;
	unsigned int max_sectors;
	unsigned int max_segments;
	unsigned int max_segment_size;
	unsigned int logical_block_size;
	unsigned int physical_block_size;
	unsigned int io_min, io_opt;
	unsigned int dma_alignment;
	unsigned long virt_boundary_mask;
	unsigned int features;
};
#define BLK_FEAT_WRITE_CACHE (1u << 0)
#define BLK_FEAT_FUA (1u << 1)
#define BLK_FEAT_ROTATIONAL (1u << 2)

struct request_queue {
	struct queue_limits limits;
	void *queuedata;
	unsigned int rq_timeout;
	unsigned long queue_flags;
};
static inline void blk_queue_rq_timeout(struct request_queue *q, unsigned int timeout) { q->rq_timeout = timeout; }
static inline unsigned int queue_max_hw_sectors(const struct request_queue *q) { return q->limits.max_hw_sectors; }
static inline unsigned int queue_logical_block_size(const struct request_queue *q) { return q->limits.logical_block_size ? q->limits.logical_block_size : 512; }
static inline void blk_queue_flag_set(unsigned int flag, struct request_queue *q) { q->queue_flags |= 1ul << flag; }
static inline void blk_queue_flag_clear(unsigned int flag, struct request_queue *q) { q->queue_flags &= ~(1ul << flag); }
#define QUEUE_FLAG_NOMERGES 3
#define QUEUE_FLAG_SAME_COMP 4
#define QUEUE_FLAG_SAME_FORCE 12

struct block_device;
#define BDEVNAME_SIZE 32
