/* LinuxKPI: the nouveau driver Kconfig (included first by every nouveau source). */
#pragma once
#define CONFIG_DRM_NOUVEAU 1
#define CONFIG_ACPI 1
#define CONFIG_NOUVEAU_DEBUG 3
#define CONFIG_NOUVEAU_DEBUG_DEFAULT 3
/* GSP-RM firmware: opt-in on Turing and Ampere (boot option
 * nouveau.config=NvGspRm=1), as in Linux 6.12. On by default it failed on
 * real hardware (GTX 1650: booter-load -5), and probing the half-started
 * GPU again froze the machine. Ada always uses it. */
/* #define CONFIG_DRM_NOUVEAU_GSP_DEFAULT 1 */
#include <linux/acpi.h>
#include <linux/scatterlist.h>
