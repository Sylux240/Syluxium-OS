/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: PM QoS requests (CPU latency limits): accepted, no effect. */
struct pm_qos_request { int value; };
struct dev_pm_qos_request { int value; };
#define PM_QOS_DEFAULT_VALUE (-1)
#define cpu_latency_qos_add_request(r, v) do { (void)(r); (void)(v); } while (0)
#define cpu_latency_qos_update_request(r, v) do { (void)(r); (void)(v); } while (0)
#define cpu_latency_qos_remove_request(r) do { (void)(r); } while (0)
#define cpu_latency_qos_request_active(r) false
#define dev_pm_qos_add_request(d, r, t, v) 0
#define dev_pm_qos_remove_request(r) 0
