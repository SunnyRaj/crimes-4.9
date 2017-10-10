#include <stdio.h>
#include <stdlib.h>

#define DEBUG 1

#define DEBUG_PAUSE()                                                       \
    do {                                                                    \
        if (DEBUG) {                                                        \
            printf("[%d] Hit [ENTER] to continue exec...\n", __LINE__);     \
                char enter = 0;                                             \
                while (enter != '\r' && enter != '\n') {                    \
                    enter = getchar();                                      \
                }                                                           \
        }                                                                   \
    } while(0)

int main()
{
    int *ptr_no_monitor = malloc(4096);
    int *ptr_monitor = malloc(4096);
    printf("PID = %d\n", getpid());
    printf("Value of canary list %lu\n", (unsigned long)ptr_monitor);

    fprintf(stdout, "\n$$$ Buffers allocated, ready to be hacked!\n");
    fprintf(stdout, "\n$$$ The following should not trigger an event\n");

    DEBUG_PAUSE();

    ptr_no_monitor[0] = 111;

    fprintf(stdout, "\n$$$ An Event should be triggered now\n");

    DEBUG_PAUSE();

    ptr_monitor[0] = 222;

    return 0;
}
