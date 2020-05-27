/**************************************************************************
 *
 *  File:       bpf.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      eBPF program tracing
 *
 *  Notes:      Use eBPF progrmas to detect following events, related to
 *              tracee:
 *              - page fault
 *              - mmap entry and exit (need exit to get actual address)
 *              - munmap entry (don't care about exit)
 *              - brk entry (don't care about exit)
 *
 **************************************************************************
 *
 *  Copyright (C) 2017-2020 Ilya Caramishev (flightrec@qrdl.com)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 **************************************************************************/
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <linux/version.h>
#include <bcc/libbpf.h>
#include <bcc/perf_reader.h>
#include <sys/sysinfo.h>

#include "flightrec.h"
#include "eel.h"
#include "bpf.h"

/* there are 3 version of libbpf API for bpf_attach_tracepoint() with 8, 5 and 3 args, so below is a hacky way
   to deal with it. It would be better to use some API version, but there isn't one */
typedef void* (*eight_args)(int, const char*, const char*, int, int, int, perf_reader_cb, void *);
typedef void* (*five_args) (int, const char*, const char*,                perf_reader_cb, void *);
typedef int   (*three_args)(int, const char*, const char*);

#define BPF_ATTACH_TRACEPOINT(FD,CAT,NAME) _Generic(bpf_attach_tracepoint, \
    eight_args:     ({ eight_args tmp = (void *)&bpf_attach_tracepoint; tmp((FD),(CAT),(NAME),pid,-1,-1,NULL,NULL); }),\
    five_args:      ({ five_args  tmp = (void *)&bpf_attach_tracepoint; tmp((FD),(CAT),(NAME),NULL,NULL); }), \
    three_args:     ({ three_args tmp = (void *)&bpf_attach_tracepoint; tmp((FD),(CAT),(NAME)); }) \
)

static struct bpf_insn *get_bpf_program(int pid, int map_fd, int event_type, int payload_offset, int *size);
static void *bpf_poller(void *);
#ifndef LOST_CB_ARGS
#define LOST_CB_ARGS	unsigned long count
#endif
static void lost_event(LOST_CB_ARGS);

static int fd_count;
static int fds_to_close[16];
static int cpu_count;
static struct perf_reader **reader;
static pthread_t worker_thread;


/**************************************************************************
 *
 *  Function:   bpf_start
 *
 *  Params:     pid - pid of program to trace
 *              callback - function to call on receipt of 
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Load BPF programs for all traced events, init
 *              kernelspace -> userspace comms, start thread for consuming
 *              events from BPF programs
 *
 **************************************************************************/
int bpf_start(pid_t pid, void (* callback)(void *, void *, int)) {
    int map_fd = bpf_create_map(
            BPF_MAP_TYPE_PERF_EVENT_ARRAY,
            "perf_map",
            sizeof(uint32_t),   // key size
            sizeof(uint32_t),   // value size
            65536,              // max entries - 1 MB array ought to be enough
            0);                 // flags
    if (map_fd < 0) {
        ERR("Failed to create map: %s", strerror(errno));
        return FAILURE;
    }
    fds_to_close[fd_count++] = map_fd;

    int prog_size;
    struct bpf_insn *program;
    char log[4096];

#define BPF_PROG_LOAD(EVT, CATEGORY, TRACEPOINT, OFFSET) do { \
    program = get_bpf_program(pid, map_fd, EVT, OFFSET, &prog_size); \
    int fd = bpf_prog_load( \
            BPF_PROG_TYPE_TRACEPOINT, \
            TRACEPOINT, \
            program, \
            prog_size, \
            "GPL", \
            LINUX_VERSION_CODE, \
            0, \
            log, \
            sizeof(log) \
    ); \
    if (fd < 0) { \
        ERR("BPF program load failed: %s", strerror(errno)); \
        return FAILURE; \
    } \
    fds_to_close[fd_count++] = fd; \
    if (!BPF_ATTACH_TRACEPOINT( \
            fd, \
            CATEGORY, \
            TRACEPOINT \
        )) { \
        ERR("Attaching tracepoint failed: %s", strerror(errno)); \
        return FAILURE; \
    } \
} while (0)

    /* load BPF programs */
    BPF_PROG_LOAD(BPF_EVT_PAGEFAULT, "exceptions", "page_fault_user", 8);
    BPF_PROG_LOAD(BPF_EVT_MMAPENTRY, "syscalls", "sys_enter_mmap", 24);
    BPF_PROG_LOAD(BPF_EVT_MMAPEXIT, "syscalls", "sys_exit_mmap", 16);
    BPF_PROG_LOAD(BPF_EVT_MUNMAP, "syscalls", "sys_enter_munmap", 16);
    BPF_PROG_LOAD(BPF_EVT_BRK, "syscalls", "sys_exit_brk", 16);
    BPF_PROG_LOAD(BPF_EVT_SIGNAL, "signal", "signal_generate", 8);

    /* specifying any CPU (value -1) doesn't work, so need to create a reader for every CPU */
    cpu_count = get_nprocs();
    reader = malloc(sizeof(*reader) * cpu_count);

    for (int cpu = 0; cpu < cpu_count; cpu++) {
        reader[cpu] = bpf_open_perf_buffer(
                callback,               // perf event read callback
                &lost_event,            // lost events callback
                NULL,
                pid,                    // pid, seems to be ignored
                cpu,                    // CPU
                256);                   // page count
        if (!reader) {
            ERR("Error creating perf event buffer: %s", strerror(errno));
            return FAILURE;
        }

        int reader_fd = perf_reader_fd(reader[cpu]);
        if (bpf_update_elem(map_fd, &cpu, &reader_fd, BPF_ANY) < 0) {
            ERR("Error registering CPU %d: %s", cpu, strerror(errno));
            return FAILURE;
        }
    }

    int ret = pthread_create(&worker_thread, NULL, bpf_poller, NULL);
    if (ret) {
        ERR("Cannot start thread for receiving perf events: %s", strerror(ret));
        return FAILURE;
    }
    pthread_setname_np(worker_thread, "fr_bpf");

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   bpf_stop
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Stop worker thread, unload BPF programs, release resources
 *
 **************************************************************************/
void bpf_stop(void) {
    pthread_cancel(worker_thread);

    for (int i = 0; i < cpu_count; i++) {
        perf_reader_free(reader[i]);
    }

    for (int i = 0; i < fd_count; i++) {
        close(fds_to_close[i]);
    }
}


/**************************************************************************
 *
 *  Function:   bpf_poller
 *
 *  Params:     unused
 *
 *  Return:     there is no normal exit from this loop, it must be
 *              terminated externally by pthread_cancel())
 *
 *  Descr:      Worker thread function, poll for perf events from BPF
 *              programs
 *
 **************************************************************************/
void *bpf_poller(void *unused) {
    (void)unused;
    /* there is no normal exit from this loop, it runs until thread is cancelled externally */
    for (;;) {
        perf_reader_poll(cpu_count, reader, -1);
    }

    return NULL;
}


/* macros for BPF bytecode in standard includes are missing function call and bitwise ops, so I roll my own ones */
#ifndef BPF_CALL_FUNC
#define BPF_CALL_FUNC(FUNC)   ((struct bpf_insn) { \
      .code    = BPF_JMP | BPF_CALL, \
      .imm     = FUNC, \
})
#endif
#ifndef BPF_BITWISE32_REG
#define BPF_BITWISE32_REG(OP, DST, SRC)   ((struct bpf_insn) { \
      .code    = BPF_MISC | BPF_X | OP, \
      .dst_reg = DST, \
      .src_reg = SRC \
})
#endif
/**************************************************************************
 *
 *  Function:   get_bpf_program
 *
 *  Params:     pid - pid of program to trace
 *              map_fd - fd of BPF_MAP_TYPE_PERF_EVENT_ARRAY map to deliver
 *                       perf events
 *              event_type - on of BPF_EVT_XXX contants
 *              payload_offset - offset of 8-byte trace event element to
 *                               send as payload
 *              size - where to store program szie (in bytes)
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Get BPF program to process specified event
 *
 **************************************************************************/
struct bpf_insn *get_bpf_program(int pid, int map_fd, int event_type, int payload_offset, int *size) {
    /* skeleton BPF program, values for PID, event type, map fd and payload offset must be specified */
    static struct bpf_insn prog[] = {
        /* save context */
        BPF_MOV64_REG(BPF_REG_6, BPF_REG_1),                // r6 = r1

        /* get event originator and compare it with supplied pid */
        BPF_CALL_FUNC(BPF_FUNC_get_current_pid_tgid),
        BPF_LD_IMM64_RAW(BPF_REG_1, BPF_REG_0, 0xFFFFFFFF00000000),
        BPF_BITWISE32_REG(BPF_AND, BPF_REG_0, BPF_REG_1),   // r0 &= r1
        BPF_LD_IMM64_RAW(BPF_REG_1, BPF_REG_0, 0),          // r1 = pid (0 is a placeholder)
        BPF_JMP_REG(BPF_JNE, BPF_REG_0, BPF_REG_1, 13),     // if (r1 != r0) goto exit (13 is nr of instr to skip)

        /* get address from event context (offset 8) and store it into the send buffer */
        BPF_MOV64_IMM(BPF_REG_1, 0),                        // r1 = event_type (0 is a placeholder)
        BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_1, -16),    // *(r10-16) = r1
        BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_6, 0),       // r1 = *(r6+offset) (0 is a placeholder)
        BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_1, -8),     // *(r10-8) = r1

        /* prepare call params - context (r1), map fd (r2), key (r3), buffer address (r4), buffer size (r5) */
        BPF_LD_MAP_FD(BPF_REG_2, 0),                        // r2 = map_fd (0 is a placeholder)
        BPF_MOV64_REG(BPF_REG_4, BPF_REG_10),               // r4 = r10
        BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, -16),             // r4 -= 16
        BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),                // r1 = r6
        BPF_LD_IMM64_RAW(BPF_REG_3, BPF_REG_0, 0x00000000FFFFFFFF), // r3 = -1
        BPF_MOV64_IMM(BPF_REG_5, 16),                        // r5 = 16
        BPF_CALL_FUNC(BPF_FUNC_perf_event_output),

        /* set exitcode and exit */
        BPF_MOV64_IMM(BPF_REG_0, 0),                        // r0 = 0
        BPF_EXIT_INSN()
    };

    prog[6].imm = pid;
    prog[8].imm = event_type;
    prog[10].off = payload_offset;
    prog[12].imm = map_fd;
    *size = sizeof(prog);
    return prog;
}

// TODO: do I really need it?
void lost_event(LOST_CB_ARGS) {
    WARN("%lu events lost\n", count);
}

#ifdef UNITTEST

static void process_event(void *cb_cookie, void *raw, int raw_size) {
    static int counter;
    struct bpf_event *event = raw;
    int hi, lo;

    switch (event->type) {
        case BPF_EVT_PAGEFAULT:
            printf("Page fault at address %" PRIx64 "\n", event->payload);
            break;
        case BPF_EVT_MMAPENTRY:
            printf("Called mmap for size %" PRIu64 "\n", event->payload);
            break;
        case BPF_EVT_MMAPEXIT:
            printf("mmap returned address %" PRIx64 "\n", event->payload);
            break;
        case BPF_EVT_SIGNAL:
            printf("[%d] child got signal %ld\n", ++counter, event->payload);
            break;
        default:
            printf("Invalid event type %" PRId64 "\n", event->type);
    }
}

FILE *logfd;

int main(int argc, char *argv[]) {
    logfd = stderr;
    if (argc < 2) {
        printf("Usage: %s <program>\n", argv[0]);
        return EXIT_FAILURE;
    }
    pid_t pid = fork();
    if (!pid) {
        sleep(1);
        char **args = malloc(2 * sizeof(*args));
        args[0] = argv[1];
        args[1] = NULL;
        execvp(args[0], args);
        printf("Error executing program\n");
        exit(1);
    }

    printf("Child PID is %d\n", pid);

    // do "echo 4 >/proc/<pid>/clear_refs" in parallel to force page faults
    if (SUCCESS != bpf_start(pid, process_event)) {
        fprintf(stderr, "Failed\n");
        return EXIT_FAILURE;
    }

    int waitstatus;
    waitpid(pid, &waitstatus, 0);

    return EXIT_SUCCESS;
}

#endif
