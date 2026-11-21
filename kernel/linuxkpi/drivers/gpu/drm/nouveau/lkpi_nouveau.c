// SPDX-License-Identifier: MIT
/*
 * LinuxKPI: the parts of nouveau Syluxium leaves out. The VM_BIND / EXEC
 * uAPI (nouveau_uvmm.c, nouveau_exec.c: used by the NVK Vulkan driver, built
 * on maple trees and drm_gpuvm) is not built; OpenGL (Mesa's nouveau gallium
 * driver) uses the classic channel/pushbuf interface, which is.
 */
#include "nouveau_drv.h"
#include "nouveau_exec.h"
#include "nouveau_uvmm.h"

int nouveau_uvmm_ioctl_vm_init(struct drm_device *dev, void *data, struct drm_file *file_priv) { return -ENOSYS; }
int nouveau_uvmm_ioctl_vm_bind(struct drm_device *dev, void *data, struct drm_file *file_priv) { return -ENOSYS; }
int nouveau_exec_ioctl_exec(struct drm_device *dev, void *data, struct drm_file *file_priv) { return -ENOSYS; }
void nouveau_uvmm_fini(struct nouveau_uvmm *uvmm) { }
void nouveau_uvmm_bo_map_all(struct nouveau_bo *nvbo, struct nouveau_mem *mem) { }
void nouveau_uvmm_bo_unmap_all(struct nouveau_bo *nvbo) { }
void drm_gpuvm_bo_evict(struct drm_gpuvm_bo *vm_bo, bool evict) { }
