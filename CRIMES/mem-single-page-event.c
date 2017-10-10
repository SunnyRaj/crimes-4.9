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
#include <sys/time.h>

#include <assert.h>
#include <arpa/inet.h>
#include <inttypes.h>

#include <libvmi/libvmi.h>
#include <libvmi/events.h>

#define DEBUG 0
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

char * ffr2e = "/home/zhen/ffr2e";        //Linux Pipe
char * ffe2r = "/home/zhen/ffe2r";

int fdr2e = 0;             //Linux Pipe remus to event-monitoring
int fde2r = 0;            //Linux Pipe event-monitoring to remus

static int interrupted = 0;
vmi_event_t mem_event;
typedef long long NANOSECONDS;
typedef struct timespec TIMESPEC;

static void print_mem_event(vmi_event_t *event);
static inline NANOSECONDS ns_timer(void);
static void close_handler(int sig);
event_response_t mem_event_cb(vmi_instance_t vmi, vmi_event_t *event);
event_response_t step_cb(vmi_instance_t vmi, vmi_event_t *event);

struct timeval tv;

struct vmi_requirements
{
	uint64_t *st_addr;
	uint64_t *en_addr;
};

int main(int argc, char **argv)
{
    struct vmi_requirements vmi_req;
    status_t status = VMI_SUCCESS;
    vmi_instance_t vmi = NULL;
    struct sigaction act;
    char* vm_name = NULL;
    unsigned long pid = 0UL;
    addr_t vaddr = 0ULL;
    addr_t paddr = 0ULL;
    uint64_t canary = 0;
    uint64_t *buf = malloc(sizeof(uint64_t));

    mkfifo(ffr2e, 0666);        //Create Pipe remus to event-monitoring
    fdr2e = open(ffr2e, O_RDONLY);      //Open Pipe remus to event-monitoring for Read
    fde2r = open(ffe2r, O_WRONLY);      //open Pipe event-monitoring to remus for Write

//    if (argc < 4) {
//        fprintf(stderr, "Usage: mem-event <name of VM> <pid> <vaddr>\n");
//        exit(1);
//    }
      if (argc < 3) {
          fprintf(stderr, "Usage: mem-event <name of VM> <pid>\n");
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
//    vaddr = strtoull(argv[3], NULL, 10);

//    fprintf(stdout,
//            "Attempting to monitor vaddr %lx in PID %lx on VM %s",
//            vaddr,
//            pid,
//            vm_name);

    fprintf(stdout,"Attempting to monitor PID %lx on VM %s", pid, vm_name);

    fprintf(stdout, "[TIMESTAMP] Received PID, initializing VMI. %lld ns\n", ns_timer());



    DEBUG_PAUSE();

    status = vmi_init_complete(&vmi, vm_name, VMI_INIT_DOMAINNAME | VMI_INIT_EVENTS,
                          NULL, VMI_CONFIG_GLOBAL_FILE_ENTRY, NULL, NULL);
    if (status == VMI_FAILURE) {
        fprintf(stdout, "Failed to init LibVMI! :( \n");
        return 1;
    } else {
        fprintf(stdout, "LibVMI init success! :) \n");
    }

    printf("Process ID is %lu\n", pid);
    read(fdr2e, buf, sizeof(void *));
    fprintf(stderr,"Address of canary list received from Save: %lu\n", *buf);
    vmi_req.st_addr = buf;
    vaddr = *(vmi_req.st_addr);
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

	if (status != VMI_SUCCESS) {
		fprintf(stdout, "Error waiting for events...DIE! %m\n");
		interrupted = -1;
	}

	vmi_read_addr_va(vmi, vaddr, pid, &canary);	
	//fprintf(stdout, "The canary value: %lu\n", canary);
	if (canary != 100) {
	    fprintf (stdout, "Wrong Canary Detected at Virtual Address: %lx\n", vaddr);
	    write(fde2r, "False", 5);
	    fsync(fde2r);
	    vmi_pause_vm(vmi);
	}
	else {
	    fprintf(stdout, "Canary Check Passed!\n");
	    write(fde2r, "True", 4);
	    fsync(fdr2e);
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

event_response_t
mem_event_cb(vmi_instance_t vmi, vmi_event_t *event)
{
    status_t status = VMI_SUCCESS;

    fprintf(stdout, "[TIMESTAMP] Mem event found on vaddr. %lld ns", ns_timer());

    print_mem_event(event);

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


static void
print_mem_event(vmi_event_t *event)
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
