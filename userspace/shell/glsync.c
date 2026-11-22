// libsyluxium-glsync.so: preloaded into the Syluxium shell's compositor
// (Wayfire) when it draws in software (llvmpipe on the firmware
// framebuffer). wlroots ends each frame with glFlush() and hands the buffer
// to the display right away, counting on the GPU driver's implicit
// synchronisation; llvmpipe's rendering threads have no fences, so the
// display copied half-drawn frames, and with partial redraws those parts
// stayed stale on the screen. Here glFlush() waits for the frame (glFinish),
// so the copy always sees a finished picture.
//
// Built without libc: glFinish comes from libGLESv2 and unsetenv from the
// C library, both already loaded by the compositor. The preload is taken
// out of the environment at once, so the programs Wayfire starts (panel,
// apps) don't inherit it.
void glFinish(void);
int unsetenv(const char *name);

__attribute__((visibility("default"))) void glFlush(void) {
    glFinish();
}

__attribute__((constructor)) static void drop_preload(void) {
    unsetenv("LD_PRELOAD");
}
