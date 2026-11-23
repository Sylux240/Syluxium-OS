/* LinuxKPI: the nouveau driver Kconfig (included first by every nouveau source). */
#pragma once
#define CONFIG_DRM_NOUVEAU 1
#define CONFIG_ACPI 1
#define CONFIG_NOUVEAU_DEBUG 3
#define CONFIG_NOUVEAU_DEBUG_DEFAULT 3
/* GSP-RM firmware by default on Turing and Ampere too (Ada always uses it);
 * nouveau_drm.c probes again without it if it fails to start. */
#define CONFIG_DRM_NOUVEAU_GSP_DEFAULT 1
#include <linux/acpi.h>
#include <linux/scatterlist.h>
