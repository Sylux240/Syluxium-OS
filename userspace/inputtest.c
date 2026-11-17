// Lists /dev/input devices (via evdev ioctls) and prints events for a few
// seconds: what a Wayland compositor or game sees.
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 6;
    struct pollfd fds[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[80] = "?";
        struct input_id id;
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        ioctl(fd, EVIOCGID, &id);
        unsigned long evbits = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits);
        printf("%s: \"%s\" bus %#x, %s%s\n", path, name, id.bustype,
               (evbits & (1 << EV_KEY)) ? "keys " : "", (evbits & (1 << EV_REL)) ? "relative-axes" : "");
        fds[n].fd = fd;
        fds[n].events = POLLIN;
        n++;
    }
    int keys = 0, motions = 0, buttons = 0, wheel = 0;
    long dx = 0, dy = 0;
    time_t end = time(NULL) + seconds;
    while (time(NULL) < end) {
        if (poll(fds, n, 500) <= 0) continue;
        for (int i = 0; i < n; i++) {
            struct input_event ev[64];
            ssize_t r = read(fds[i].fd, ev, sizeof(ev));
            for (int k = 0; r > 0 && k < r / (ssize_t)sizeof(ev[0]); k++) {
                if (ev[k].type == EV_KEY && ev[k].code >= BTN_LEFT && ev[k].code <= BTN_TASK) {
                    if (ev[k].value == 1) { buttons++; printf("  button %#x pressed\n", ev[k].code); }
                } else if (ev[k].type == EV_KEY && ev[k].value == 1) {
                    keys++;
                    printf("  key %d down\n", ev[k].code);
                } else if (ev[k].type == EV_REL) {
                    motions++;
                    if (ev[k].code == REL_X) dx += ev[k].value;
                    if (ev[k].code == REL_Y) dy += ev[k].value;
                    if (ev[k].code == REL_WHEEL) wheel += ev[k].value;
                }
            }
        }
    }
    printf("inputtest: %d key presses, %d motion events (dx=%ld dy=%ld), %d clicks, wheel %d\n", keys, motions, dx,
           dy, buttons, wheel);
    return 0;
}
