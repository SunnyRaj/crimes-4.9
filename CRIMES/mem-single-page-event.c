#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include <libvmi/libvmi.h>
#include <libvmi/events.h>

#define DEBUG 1
#define PIPE_BUF_TO_MEM_FD "/tmp/buf_to_mem"

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


static int interrupted = 0;
vmi_event_t mem_event;
typedef long long NANOSECONDS;
typedef struct timespec TIMESPEC;

static void print_mem_event(vmi_event_t *event);
static inline NANOSECONDS ns_timer(void);
static void close_handler(int sig);
event_response_t mem_event_cb(vmi_instance_t vmi, vmi_event_t *event);
event_response_t step_cb(vmi_instance_t vmi, vmi_event_t *event);

int main(int argc, char **argv)
{
    status_t status = VMI_SUCCESS;
    vmi_instance_t vmi = NULL;
    struct sigaction act;
    char* vm_name = NULL;
    unsigned long pid = 0UL;
    addr_t vaddr = 0ULL;
    addr_t paddr = 0ULL;
    uint64_t canary = 0;

    if (argc < 4) {
        fprintf(stderr, "Usage: mem-event <name of VM> <pid> <vaddr>\n");
        exit(1);
    }

    fprintf(stdout, "Started Mem-Events Program\n");

    act.sa_handler = close_handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGHUP,   &act, NULL);
    sigaction(SIGTERM,  &act, NULL);
    sigaction(SIGINT,   &act, NULL);
    sigaction(SIGALRM,  &act, NULL);
    sigaction(SIGKILL,  &act, NULL);

    vm_name = argv[1];
    pid = strtoul(argv[2], NULL, 10);
    vaddr = strtoull(argv[3], NULL, 10);

    fprintf(stdout,
            "Attempting to monitor vaddr %lx in PID %lx on VM %s",
            vaddr,
            pid,
            vm_name);

    fprintf(stdout, "[TIMESTAMP] Received vaddr, initializing VMI. %lld ns\n", ns_timer());

    DEBUG_PAUSE();

    status = vmi_init_complete(&vmi, vm_name, VMI_INIT_DOMAINNAME | VMI_INIT_EVENTS,
                          NULL, VMI_CONFIG_GLOBAL_FILE_ENTRY, NULL, NULL);
    if (status == VMI_FAILURE) {
        fprintf(stdout, "Failed to init LibVMI! :( \n");
        return 1;
    } else {
        fprintf(stdout, "LibVMI init success! :) \n");
    }

    vmi_translate_uv2p(vmi, vaddr, pid, &paddr);
    if (paddr == 0) {
        fprintf(stdout, "Failed to translate uv2p...DIE! %m\n");
        status = VMI_FAILURE;
        goto cleanup;
    }

    fprintf(stdout, "Monitoring paddr %lx on \"%s\"\n", paddr, vm_name);

    fprintf(stdout,
            "Preparing memory event to monitor PA 0x%lx, page 0x%lx\n",
            paddr,
            (paddr >> 12));

    memset(&mem_event, 0, sizeof(vmi_event_t));
    SETUP_MEM_EVENT(&mem_event,
                    (paddr >> 12),
                    VMI_MEMACCESS_RW,
                    mem_event_cb,
                    0);

    status = vmi_register_event(vmi, &mem_event);
    if (status == VMI_FAILURE) {
        fprintf(stdout, "Failed to register mem event...DIE! %m\n");
        goto cleanup;
    }

    while (!interrupted) {
        status = vmi_events_listen(vmi, 500);

	      vmi_read_addr_va(vmi, vaddr, pid, &canary);	
	      fprintf(stderr, "The canary value: %lu\n", canary);
	      if (canary != 100) {
	      fprintf (stdout, "Wrong Canary Detected at Virtual Address: %lx\n Danger!!!\n Danger!!!\n Danger!!!", vaddr);
        DEBUG_PAUSE();
	      }
        if (status != VMI_SUCCESS) {
            fprintf(stdout, "Error waiting for events...DIE! %m\n");
            interrupted = -1;
        }
    }

cleanup:
    fprintf(stdout, "Finished mem-event test\n");

    vmi_destroy(vmi);

    if (status == VMI_FAILURE) {
        fprintf(stdout, "Exit with status VMI_FAILURE\n");
    } else {
        fprintf(stdout, "Exit with status VMI_SUCCESS\n");
    }

    return status;
}

event_response_t step_callback(vmi_instance_t vmi, vmi_event_t *event) {
    printf("Re-registering event\n");
    vmi_register_event(vmi, event);
    return 0;
}

event_response_t
mem_event_cb(vmi_instance_t vmi, vmi_event_t *event)
{
    status_t status = VMI_SUCCESS;

    fprintf(stderr, "[TIMESTAMP] Mem event found on vaddr. %lld ns\n", ns_timer());

    print_mem_event(event);

    status = vmi_step_event(vmi, event, event->vcpu_id, 1, step_callback);
    DEBUG_PAUSE();
    status = vmi_clear_event(vmi, event, NULL);
    if (status == VMI_FAILURE) {
        fprintf(stdout, "Failed to clear mem event in cb...DIE! %m\n");
        return 1;
    }

//    status = vmi_step_event(vmi,
//                            event,
//                            event->vcpu_id,
//                            1,
//                            NULL);
//    if (status == VMI_FAILURE) {
//        fprintf(stdout, "Failed to step event...DIE! %m\n");
//        return 1;
//    }

    //interrupted = 6;

    return 0;
}


void print_mem_event(vmi_event_t *event)
{
    fprintf(stdout,
            "PAGE %" PRIx64 " ACCESS: %c%c%c for GFN %" PRIx64 " (offset %06" PRIx64 ") gla %016" PRIx64 " (vcpu %u)\n",
            (event->mem_event.gfn >> 12),
            (event->mem_event.out_access & VMI_MEMACCESS_R) ? 'r' : '-',
            (event->mem_event.out_access & VMI_MEMACCESS_W) ? 'w' : '-',
            (event->mem_event.out_access & VMI_MEMACCESS_X) ? 'x' : '-',
            event->mem_event.gfn,
            event->mem_event.offset,
            event->mem_event.gla,
            event->vcpu_id
            );
}

static inline NANOSECONDS
ns_timer(void)
{
    TIMESPEC curr_time;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);

    return (NANOSECONDS) (curr_time.tv_sec * 1000000000LL) +
           (NANOSECONDS) (curr_time.tv_nsec);
}

static void
close_handler(int sig)
{
    interrupted = sig;
}
