/* LinuxKPI: traffic-control hardware offload descriptions (Linux
 * net/pkt_sched.h, uapi pkt_sched.h) that time-sensitive-networking NICs
 * (igc) handle in ndo_setup_tc. Syluxium has no tc qdiscs, so nothing ever
 * asks for these offloads; the types let the drivers compile unchanged. */
#pragma once

#include <linux/lkpi_all.h>

#define TC_QOPT_BITMASK 15
#define TC_QOPT_MAX_QUEUE 16
#define TC_MAX_QUEUE 16

enum { TC_TAPRIO_CMD_SET_GATES = 0x00, TC_TAPRIO_CMD_SET_AND_HOLD = 0x01, TC_TAPRIO_CMD_SET_AND_RELEASE = 0x02 };
enum { TC_MQPRIO_MODE_DCB, TC_MQPRIO_MODE_CHANNEL };
enum { TC_MQPRIO_SHAPER_DCB, TC_MQPRIO_SHAPER_BW_RATE };

struct netlink_ext_ack;

struct tc_mqprio_qopt {
    __u8 num_tc;
    __u8 prio_tc_map[TC_QOPT_BITMASK + 1];
    __u8 hw;
    __u16 count[TC_QOPT_MAX_QUEUE];
    __u16 offset[TC_QOPT_MAX_QUEUE];
};

struct tc_query_caps_base {
    enum tc_setup_type type;
    void *caps;
};

struct tc_cbs_qopt_offload {
    u8 enable;
    s32 queue;
    s32 hicredit;
    s32 locredit;
    s32 idleslope;
    s32 sendslope;
};

struct tc_etf_qopt_offload {
    u8 enable;
    s32 queue;
};

struct tc_mqprio_caps {
    bool validate_queue_counts : 1;
};

struct tc_mqprio_qopt_offload {
    struct tc_mqprio_qopt qopt;
    struct netlink_ext_ack *extack;
    u16 mode;
    u16 shaper;
    u32 flags;
    u64 min_rate[TC_QOPT_MAX_QUEUE];
    u64 max_rate[TC_QOPT_MAX_QUEUE];
    unsigned long preemptible_tcs;
};

struct tc_taprio_caps {
    bool supports_queue_max_sdu : 1;
    bool gate_mask_per_txq : 1;
    bool broken_mqprio : 1;
};

enum tc_taprio_qopt_cmd { TAPRIO_CMD_REPLACE, TAPRIO_CMD_DESTROY, TAPRIO_CMD_STATS, TAPRIO_CMD_QUEUE_STATS };

struct tc_taprio_qopt_stats {
    u64 window_drops;
    u64 tx_overruns;
};

struct tc_taprio_qopt_queue_stats {
    int queue;
    struct tc_taprio_qopt_stats stats;
};

struct tc_taprio_sched_entry {
    u8 command;
    u32 gate_mask;
    u32 interval;
};

struct tc_taprio_qopt_offload {
    enum tc_taprio_qopt_cmd cmd;
    union {
        struct tc_taprio_qopt_stats stats;
        struct tc_taprio_qopt_queue_stats queue_stats;
        struct {
            struct tc_mqprio_qopt_offload mqprio;
            struct netlink_ext_ack *extack;
            ktime_t base_time;
            u64 cycle_time;
            u64 cycle_time_extension;
            u32 max_sdu[TC_MAX_QUEUE];
            size_t num_entries;
            struct tc_taprio_sched_entry entries[];
        };
    };
};

static inline struct tc_taprio_qopt_offload *taprio_offload_get(struct tc_taprio_qopt_offload *offload)
{
    return NULL;
}
static inline void taprio_offload_free(struct tc_taprio_qopt_offload *offload) { }
