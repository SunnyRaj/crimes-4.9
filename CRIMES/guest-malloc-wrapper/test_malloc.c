#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>

typedef long long NANOSECONDS;
typedef struct timespec TIMESPEC;
static int count = 0;
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


static inline NANOSECONDS ns_timer(void)
{
    TIMESPEC curr_time;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);

    return (NANOSECONDS) (curr_time.tv_sec * 1000000000LL) +
           (NANOSECONDS) (curr_time.tv_nsec);
}

void *rand_string(char *str, size_t size)
{
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK...";
    size_t n = 0;
    if (size) {
        --size;
        for (n = 0; n < size; n++) {
            int key = rand() % (int) (sizeof charset - 1);
            str[n] = charset[key];
        }
        str[size] = '\0';
    }
    return str;
}

void exploit(char *ptr1)
{
    count++;
    char *rand_str;

    if (count <= 3) {
	    rand_str = malloc(8);
	    rand_str = rand_string(rand_str, 8);
    } else {
            rand_str = malloc(12);
	    rand_str = rand_string(rand_str, 12);
    }

    printf("Timestamp 1 %llu\n", ns_timer());
    strcpy(ptr1, rand_str);
    //strcpy(ptr1, "aaaaaaaa");
    printf("Address of ptr1[10] is %p\n", &ptr1[10]); 
    printf("The value of the new string is %s\n", ptr1);

    printf("Timestamp 2 %llu\n", ns_timer());
}
int main()
{
    printf("$$$ test-malloc running with PID = %d\n", getpid());
    char *ptr1 = malloc(sizeof(char) * 10);
    while(1)
    {
        printf("Address of ptr1 is %p\n", ptr1); 

        fprintf(stdout, "\n$$$ Buffers allocated, ready to be hacked!\n");

        DEBUG_PAUSE();

        exploit(ptr1);
    }
}

