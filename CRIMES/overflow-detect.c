#include <assert.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include <libvmi/libvmi.h>
//#include <libvmi/events.h>

//#include "./xc_pipe.h"
#include <time.h>
#include <sys/time.h>

#define COUNT 2

#define vmi_read_ff "/tmp/ffr2o"
#define vmi_write_ff "/tmp/ffo2r"

#if 0
static int interrupted = 0;
static int mem_cb_count = 0;

static void print_mem_event(vmi_event_t *event);
static void close_handler(int sig);

vmi_event_t mem_event;

event_response_t mem_event_cb(vmi_instance_t vmi, vmi_event_t *event);
event_response_t step_cb(vmi_instance_t vmi, vmi_event_t *event);
#endif

/* Timer related variables */
typedef long long NANOSECONDS;
typedef struct timespec TIMESPEC;

static inline NANOSECONDS ns_timer(void)
{
    TIMESPEC curr_time;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);
    return (NANOSECONDS) (curr_time.tv_sec * 1000000000LL) +
    (NANOSECONDS) (curr_time.tv_nsec);
}

int main (int argc, char **argv)
{
    status_t status = VMI_SUCCESS;
    char* name = NULL;
    vmi_instance_t vmi;
    vmi_pid_t pid;
    addr_t vaddr = 0ULL;
    int ret_count;
    char buf[24];
    uint64_t canary = 0;

    if (argc < 4) {
        fprintf(stderr, "Usage: overflow-detect <name of VM> <pid> <vaddr>\n");
        exit(1);
    }

    name = argv[1];
    pid = strtoul(argv[2], NULL, 10);
    vaddr = strtoull(argv[3], NULL, 10);

/*
    act.sa_handler = close_handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGHUP,   &act, NULL);
    sigaction(SIGTERM,  &act, NULL);
    sigaction(SIGINT,   &act, NULL);
    sigaction(SIGALRM,  &act, NULL);
    sigaction(SIGKILL,  &act, NULL);
*/


    /*---------------------Linux Pipe---------------------------*/
    int vmi_read_fd;             //Linux Pipe 1
    int vmi_write_fd;            //Linux Pipe 2

    mkfifo(vmi_read_ff, 0666);        //Create Pipe 1
    vmi_read_fd = open(vmi_read_ff, O_RDONLY);      //Open Pipe 1 for Read
    vmi_write_fd = open(vmi_write_ff, O_WRONLY);      //open Pipe 2 for Write

    if (VMI_FAILURE ==
        vmi_init_complete(&vmi, name, VMI_INIT_DOMAINNAME, NULL,
            VMI_CONFIG_GLOBAL_FILE_ENTRY, NULL, NULL)) {
        printf("Failed to init LibVMI library.\n");
            return 1;
    }

    fprintf(stdout,
            "Attempting to monitor vaddr %lx in PID %lx on VM %s\n",
            vaddr,
            pid,
            name);

    fprintf(stdout, "[TIMESTAMP] Received vaddr, initializing VMI. %lld ns\n", ns_timer());

    printf("success to init LibVMI\n");

    while(1)
    {
        /*
         * Read the address of canary_list; buf has the address of canary_list
         */
        printf("Process ID is %lu\n", pid);
        read(vmi_read_fd, buf, 24);
        fprintf(stderr,"Signal Received from Remus: %s\n", buf);

        //vmi_req.st_addr = buf;

        //vaddr1 = *(vmi_req.st_addr);

        /*
         * Read the address at the starting address of the canary_list
         */
        ret_count = vmi_read_addr_va(vmi, vaddr, pid, &canary);
        printf("The value at virtual address: %lu is: %lu\n", vaddr, canary);
        /*
         * Read Canary from address canary_address[0]
         */
//        ret_count = vmi_read_addr_va(vmi, canary_address[0], pid, &canary);
//        printf("The value inside canary address: %lu is: %lu\n", *canary_address, canary);

        if (canary != 100)
        {
            //printf("The value at virtual address: %lu is: %lu\n", vaddr, canary);
            fprintf(stdout, "[TIMESTAMP] Canary address violated %lld ns\n", ns_timer());
            write(vmi_write_fd, "Bad", 3);             //Write to Pipe 2
            fprintf(stderr, "Overflow Detected\n");
            fsync(vmi_write_fd);
            break;
        }
        else
        {
            write(vmi_write_fd, "Good", 4);             //Write to Pipe 2
            fprintf(stdout, "Canary Check Passed!!\n");
            fsync(vmi_write_fd);
        }

    canary = 0;
    }

    //vmi_destroy(vmi);

//    close(vmi_read_fd);
//    close(vmi_write_fd);
//
//    unlink(vmi_read_ff);

cleanup:
    //vmi_destroy(vmi);

    if (status == VMI_FAILURE) {
        fprintf(stdout, "Exit with status VMI_FAILURE\n");
    } else {
        fprintf(stdout, "Exit with status VMI_SUCCESS\n");
    }

    return status;
}
