// Deliberately dereferences NULL. The kernel must kill this process with
// SIGSEGV and keep running.
#include <stdio.h>

int main(void) {
    printf("crashtest: about to write through a NULL pointer...\n");
    fflush(stdout);
    volatile int* p = 0;
    *p = 42;
    printf("crashtest: still alive?! (this is a bug)\n");
    return 0;
}
