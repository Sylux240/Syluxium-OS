/* LinuxKPI: the i915 driver's Kconfig (included first by every i915 source,
 * see the Makefile). Values are Linux's defaults (Kconfig.profile);
 * debugging, PMU/perf, GVT, PXP, userptr and error capture are off. */
#pragma once
#define CONFIG_DRM_I915 1
#define CONFIG_ACPI 1
#define CONFIG_DRM_I915_FORCE_PROBE ""
#define CONFIG_DRM_I915_REQUEST_TIMEOUT 20000
#define CONFIG_DRM_I915_FENCE_TIMEOUT 10000
#define CONFIG_DRM_I915_USERFAULT_AUTOSUSPEND 250
#define CONFIG_DRM_I915_HEARTBEAT_INTERVAL 2500
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT 640
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT_COMPUTE 7500
#define CONFIG_DRM_I915_MAX_REQUEST_BUSYWAIT 8000
#define CONFIG_DRM_I915_STOP_TIMEOUT 100
#define CONFIG_DRM_I915_TIMESLICE_DURATION 1
#include <linux/acpi.h>
