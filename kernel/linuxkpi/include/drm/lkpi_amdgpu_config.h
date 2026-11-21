/* LinuxKPI: the amdgpu driver Kconfig (included first by every amdgpu source).
 * No display core (DC) and no compute (KFD): Volcanic Islands / Polaris and
 * older run with the legacy display code. */
#pragma once
#define CONFIG_DRM_AMDGPU 1
#define CONFIG_DRM_AMDGPU_SI 1
#define CONFIG_DRM_AMDGPU_CIK 1
#define CONFIG_ACPI 1
#include <linux/acpi.h>
