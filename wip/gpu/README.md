# GPU work in progress (not built)

Linux 6.12 DRM core subset + simpledrm + dma-buf, for Phase 10 (GPU
acceleration). Moved out of `kernel/` so the build stays green; headers
(real Linux ones and LinuxKPI stubs) are already in
`kernel/linuxkpi/include/{drm,linux,uapi}`.

First compile pass (sources under kernel/linuxkpi): ~950 errors after
stubbing missing headers - the real gaps are GEM memory/mmap with
driver page faults, dma-buf/dma-fence/sync_file, the DRM file + ioctl
plumbing, and many smaller kernel APIs.

Plan: DRM core + simpledrm (KMS on the firmware framebuffer, Xorg
modesetting) -> virtio-gpu with 3D (Mesa/virgl in QEMU) -> i915
(ThinkPad UHD 620: iris, MacBook Air HD 5000: crocus).
