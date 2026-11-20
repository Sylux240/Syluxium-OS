/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct cec_notifier;
#define cec_notifier_conn_register(dev, port, info) ((struct cec_notifier *)NULL)
#define cec_notifier_conn_unregister(n) do { } while (0)
#define cec_notifier_set_phys_addr(n, pa) do { } while (0)
#define cec_notifier_set_phys_addr_from_edid(n, e) do { } while (0)
#define cec_notifier_phys_addr_invalidate(n) do { } while (0)
#define cec_fill_conn_info_from_drm(info, conn) do { } while (0)
struct cec_connector_info { int type; };
