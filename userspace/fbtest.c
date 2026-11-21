// /dev/fb0 test: query the mode, switch the console to graphics mode, draw.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
        perror("fb0");
        return 1;
    }
    printf("fbtest: %s %ux%u, %u bpp, line %u bytes, red@%u green@%u blue@%u, %u KiB\n", f.id, v.xres, v.yres,
           v.bits_per_pixel, f.line_length, v.red.offset, v.green.offset, v.blue.offset, f.smem_len / 1024);
    uint8_t* fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    int tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0 && ioctl(tty, KDSETMODE, KD_GRAPHICS) == 0)
        printf("fbtest: console in KD_GRAPHICS mode\n");
    // Background gradient (blue sky to green hill) and test boxes.
    for (unsigned y = 0; y < v.yres; y++) {
        uint32_t* row = (uint32_t*)(fb + y * f.line_length);
        for (unsigned x = 0; x < v.xres; x++) {
            unsigned r, g, b;
            if (y < v.yres * 2 / 3) { r = 40 + y * 60 / v.yres; g = 100 + y * 100 / v.yres; b = 230; }
            else { r = 60; g = 150 + (y - v.yres * 2 / 3) * 60 / v.yres; b = 50; }
            row[x] = (r << v.red.offset) | (g << v.green.offset) | (b << v.blue.offset);
        }
    }
    uint32_t colors[3] = {0xFF0000, 0x00FF00, 0x0000FF};
    for (int i = 0; i < 3; i++)
        for (unsigned y = 60; y < 160; y++) {
            uint32_t* row = (uint32_t*)(fb + y * f.line_length);
            for (unsigned x = 60 + i * 140; x < 180 + i * 140; x++) {
                uint32_t c = colors[i];
                row[x] = (((c >> 16) & 255) << v.red.offset) | (((c >> 8) & 255) << v.green.offset) | ((c & 255) << v.blue.offset);
            }
        }
    // Taskbar-like strip at the bottom with a green "start" block.
    for (unsigned y = v.yres - 30; y < v.yres; y++) {
        uint32_t* row = (uint32_t*)(fb + y * f.line_length);
        for (unsigned x = 0; x < v.xres; x++)
            row[x] = x < 100 ? ((60u << v.red.offset) | (170u << v.green.offset) | (60u << v.blue.offset))
                             : ((20u << v.red.offset) | (80u << v.green.offset) | (200u << v.blue.offset));
    }
    msync(fb, f.smem_len, MS_SYNC);
    printf("fbtest: drew a test pattern (%s)\n", argc > 1 && !strcmp(argv[1], "restore") ? "restoring text" : "left on screen");
    if (argc > 1 && !strcmp(argv[1], "restore")) {
        sleep(1);
        ioctl(tty, KDSETMODE, KD_TEXT);
    }
    return 0;
}
