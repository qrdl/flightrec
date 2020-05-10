/**************************************************************************
 *
 *  File:       run.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Execute process and record its activity
 *
 *  Notes:
 *
 **************************************************************************
 *
 *  Copyright (C) 2017-2020 Ilya Caramishev (ilya@qrdl.com)
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
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/uio.h>
#include <inttypes.h>
#include <pthread.h>
// for P_tmpdir
#ifndef __USE_XOPEN
#define __USE_XOPEN 1
#endif
#include <stdio.h>

#include "stingray.h"
#include "eel.h"
#include "dab.h"

#include "flightrec.h"
#include "record.h"
#include "mem.h"
#include "memcache.h"
#include "channel.h"
#include "workers.h"
#include "bpf.h"

#ifdef __x86_64__
#define IP(A)   A.rip
#define BP(A)   A.rbp
#else
#define IP(A)   A.eip
#define BP(A)   A.ebp
#endif

#define FUNC_FLAG_START     1
#define FUNC_FLAG_END       2

static void set_ip(pid_t, REG_TYPE ip);
static int set_breakpoints(pid_t pid);
static int process_breakpoint(pid_t pid);

static void bpf_callback(void *cookie, void *data, int data_size);

unsigned long page_size;
int fifo_fd = 0;

static struct channel *insert_step_ch;
static struct channel *insert_heap_ch;
struct channel *insert_mem_ch;          // not-static because is used in memcache.c
/* child program base address. Addresses from debug info may or may not contain base address, so if accessing child
   memory fails, assume addresses need to be adjusted by base address */
static uint64_t base_address = 0;
/* this mutex is used to sync access to cached memory between main loop and bpf_callback() */
static pthread_mutex_t cachedmem_access = PTHREAD_MUTEX_INITIALIZER;

/**************************************************************************
 *
 *  Function:   record
 *
 *  Params:     fr_path - path to FlightRec executable
 *              params - param vector to pass to exec(), first item
 *                       must contain program name
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      run process under tracer - the main loop
 *
 **************************************************************************/
int record(char *fr_path, char *params[]) {

    int pid = fork();
    if (pid) {
        /* parent */
        int wait_status;
        waitpid(pid, &wait_status, 0);      // wait for SIGTRAP from child, indicating the exec

        // create temporary FIFO to get info re dynamic memory
        char fifo_name[256];
        sprintf(fifo_name, "%s/fr_%X", P_tmpdir, pid);
        /* there is a race between parent and child in creating the fifo so handle it */
        if (mkfifo(fifo_name, S_IRUSR | S_IWUSR) && EEXIST != errno) {
            ERR("Cannot create named pipe: %s", strerror(errno));
            return FAILURE;
        }
        fifo_fd = open(fifo_name, O_RDONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            ERR("Cannot open named pipe: %s", strerror(errno));
            return FAILURE;
        }

        // set breakpoints for all known source lines in child process
        if (SUCCESS != set_breakpoints(pid)) {
            ERR("Cannot set breakpoints");
            return FAILURE;
        }
        INFO("Tracing %s", params[0]);

        /* Init worker's channels and start workers for writing to DB */
        START_WORKER(step);
        START_WORKER(heap);
        START_WORKER(mem);

        /* continue to first executable line */
        if (-1 == ptrace(PTRACE_CONT, pid, NULL, NULL)) {
            ERR("Cannot start executing child program: %s", strerror(errno));
            return FAILURE;
        }
        waitpid(pid, &wait_status, 0);      // wait for SIGTRAP from child, indicating the breakpoint
        int signum = WSTOPSIG(wait_status);
        if (WIFEXITED(wait_status) || SIGTRAP != signum) {
            ERR("Child exited right after the start");
            return FAILURE;
        }

        /* init memory cache and store initial memory content */
        if (SUCCESS != init_cache(pid)) {
            return FAILURE;
        }
        if (SUCCESS != process_breakpoint(pid)) {
            return FAILURE;
        }

        /* load BPF programs to monitor page faults, signals and mmap/munmap/brk syscalls */
        if (SUCCESS != bpf_start(pid, bpf_callback)) {
            return FAILURE;
        }

        while (-1 != ptrace(PTRACE_CONT, pid, NULL, NULL)) {
            waitpid(pid, &wait_status, 0);      // wait for SIGTRAP from child, indicating the breakpoint
            if (WIFEXITED(wait_status)) {
                INFO("child exited");
                signum = 0;
                break;                          // child exited ok
            }

            if (WIFSTOPPED(wait_status)) {
                signum = WSTOPSIG(wait_status);
                /* wait until BPF callback releases the mutex so it is safe to process memory */
                int ret = pthread_mutex_lock(&cachedmem_access);
                if (ret) {
                    ERR("Error locking mutex: %s", strerror(ret));
                    return FAILURE;
                }
                if (SUCCESS != process_breakpoint(pid)) {
                    return FAILURE;
                }
                ret = pthread_mutex_unlock(&cachedmem_access);
                if (ret) {
                    ERR("Error unlocking mutex: %s", strerror(ret));
                    return FAILURE;
                }
                if (SIGTRAP != signum) {
                    INFO("Child stopped - sig %d", signum);
                    break;
                }
            } else {
                // TODO: can we get here?
                ERR("Unsupported wait status %d", wait_status);
                return FAILURE;
            }
        }
        bpf_stop();

        INFO("Waiting for worker threads to finish");

        /* Send termination to workers and wait for workers to finish. Because workers
           flush data to DB, process them one by one, concurrency can corrupt DB */
        WAIT_WORKER(step);
        WAIT_WORKER(heap);
        WAIT_WORKER(mem);

        close(fifo_fd);
        unlink(fifo_name);

        /* TODO I don't know why but inserting of signal into DB fails with 'locked', so DB close/open helps */
        DAB_CLOSE(0);
        if (signum) {
            extern char *db_name;
            if (DAB_OK != DAB_OPEN(db_name, DAB_FLAG_NONE)) {     // already in multi-threaded mode
                return FAILURE;
            }
            if (DAB_OK != DAB_EXEC("INSERT INTO misc (key, value) VALUES ('exit_signal', 11)")) {
                ERR("Cannot store exit signal in DB");
            }
        }

    } else {
        /* child */
        if (-1 == ptrace(PTRACE_TRACEME, 0, NULL, NULL)) {
            ERR("Cannot start trace in the child - %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        char preload[256];
        sprintf(preload, "LD_PRELOAD=%s/fr_preload.so", fr_path);   // preload lib to intercept malloc() etc.
        putenv(preload);
        execvp(params[0], params);
        /* get here only in case of exec failure  */
        ERR("Cannot execute %s - %s", params[0], strerror(errno));
        exit(EXIT_FAILURE);
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   set_breakpoints
 *
 *  Params:     pid - pid of process being traced
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Set breakpoints for all known source lines
 *
 **************************************************************************/
int set_breakpoints(pid_t pid) {
    int ret = SUCCESS;
    void *cursor, *update_cursor = NULL;    // these statements are declared and used in-place because set_breakpoints() called just once
    REG_TYPE address;
    REG_TYPE int3 = 0xCC;    // INT 3

    if (DAB_OK != DAB_CURSOR_OPEN(&cursor, "SELECT "
                "address, "
                "rowid "
            "FROM "
                "statement "
            "ORDER BY "
                "file_id, "
                "line")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_BEGIN) {
        return FAILURE;
    }

    int db_stat;
    ULONG rowid;
    while (DAB_OK == (db_stat = DAB_CURSOR_FETCH(cursor, &address, &rowid))) {
        /* read one byte at specified address, remember it and replace with INT 3 instruction to cause TRAP */
        REG_TYPE instr;
        do {
            errno = 0;  // PTRACE_PEEKDATA can return anything, even -1, so use only errno for diag
            instr = ptrace(PTRACE_PEEKDATA, pid, (void *)(address+base_address), NULL);
            if (errno) {
                if ((EIO == errno || EFAULT == errno) && !base_address) {
                    /* may fail because address needs to be adjusted by base address */
                    if (SUCCESS != get_base_address(pid, &base_address)) {
                        RETCLEAN(FAILURE);
                    }
                    if (!base_address) {
                        ERR("Cannot peek at child code (base addr is zero) - %s", strerror(errno));
                        RETCLEAN(FAILURE);
                    }
                    /* save base address for further use by Examine */
                    if (DAB_OK != DAB_EXEC("INSERT INTO misc (key, value) VALUES ('base_address', ?)", base_address)) {
                        ERR("Cannot update unit base address");
                        RETCLEAN(FAILURE);
                    }
                    continue;   // repeat the attempt with non-zero base_address
                }
                ERR("Cannot peek at child code - %s", strerror(errno));
                RETCLEAN(FAILURE);
            }
	    break;
        } while (1);
        /* store original instruction in DB */
        if (!update_cursor) {
            if (DAB_OK != DAB_CURSOR_OPEN(&update_cursor,
                    "UPDATE "
                        "statement "
                    "SET "
                        "instr = ? "
                    "WHERE "
                        "rowid = ?",
                    (instr & 0xFF), rowid)) {
                RETCLEAN(FAILURE);
            }
        } else {
            DAB_CURSOR_RESET(update_cursor);
            if (DAB_OK != DAB_CURSOR_BIND(update_cursor, (instr & 0xFF), rowid)) {
                RETCLEAN(FAILURE);
            }
        }
        if (DAB_NO_DATA != DAB_CURSOR_FETCH(update_cursor)) {
            RETCLEAN(FAILURE);
        }
        /* update child code */
        instr = (instr & ~0xFF) | int3;
        if (-1 == ptrace(PTRACE_POKEDATA, pid, (void *)(address+base_address), (void *)instr)) {
            ERR("Cannot update child code - %s", strerror(errno));
            RETCLEAN(FAILURE);
        }
    }
    if (DAB_NO_DATA != db_stat) {
        RETCLEAN(FAILURE);
    }

cleanup:
    DAB_CURSOR_FREE(cursor);
    DAB_CURSOR_FREE(update_cursor);
    if (SUCCESS == ret && DAB_OK == DAB_COMMIT) {
        return SUCCESS;
    }
    DAB_ROLLBACK;

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_breakpoint
 *
 *  Params:     pid - pid of process being traced
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process the breakpoint:
 *              - store program step
 *              - check for any dynamic memory ops happened since prev step
 *              - check for any memory changes since prev step
 *
 **************************************************************************/
int process_breakpoint(pid_t pid) {
    static ULONG counter = 0;
    static ULONG func_id = 0, depth = 0, old_func_id = 0;
    int func_flag;
    int scope_id;


    /* Get registers */
    struct user_regs_struct regs;
    if (-1 == ptrace(PTRACE_GETREGS, pid, NULL, &regs)) {
        printf("Cannot read process registers - %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    REG_TYPE pc = IP(regs) - 1;       // program counter at breakpoint, before it processed the TRAP
    ULONG fid, line;
    int saved;

    DAB_CURSOR_RESET(select_line);
    if (DAB_OK != DAB_CURSOR_BIND(select_line, pc - base_address)) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_FETCH(select_line, &fid, &line, &saved, &func_id, &func_flag, &scope_id)) {
        return FAILURE;
    }

    if (func_id != old_func_id || FUNC_FLAG_START == func_flag) {
        if (FUNC_FLAG_START == func_flag) {
            depth++;        // got to the begining of the function - new function call
        }
        // depth for FUNC_FLAG_END will be decremented after processing the step
        // if not function call, we can get here as a result of return from function call or long jump
        // TODO handle long jump
        old_func_id = func_id;
    }

    /* Store new step using worker */
    ULONG step_id = ++counter;
    struct insert_step_msg *msg = malloc(sizeof(*msg));
    msg->step_id = step_id;
    msg->file_id = fid;
    msg->line = line;
    msg->depth = depth;
    msg->func_id = func_id;
    msg->regs = regs;   // regs is struct, not a pointer, so it will be copied
    ch_write(insert_step_ch, (char *)msg, sizeof(*msg));    // channel reader will free msg

    /* check for heap events happened in tracee */
    int read_status;
    struct heap_event event;
    // read() won't block if there is nothing to read
    while ((read_status = read(fifo_fd, &event, sizeof(event))) > 0) {
        /* Store heap memory event using worker */
        struct insert_heap_msg *msg = malloc(sizeof(*msg));
        msg->step_id = step_id;
        msg->address = event.address;
        if (HEAP_EVENT_ALLOC == event.type) {
            msg->size = event.size;
        } else {
            msg->size = 0;      // indicate 'free'
        }
        ch_write(insert_heap_ch, (char *)msg, sizeof(*msg));    // channel reader will free msg
    }
    if (read_status < 0 && errno != EAGAIN) {
        ERR("Error reading from pipe: %s", strerror(errno));
        return FAILURE;
    }

    if (FUNC_FLAG_START != func_flag || 1 == step_id) {
        if (SUCCESS != process_dirty(step_id)) {
            return FAILURE;
        }
        if (FUNC_FLAG_END == func_flag) {
            depth--;
        }
    }

    errno = 0;  // PTRACE_PEEKDATA can return anything, even -1, so use only errno for diag
    REG_TYPE instr = ptrace(PTRACE_PEEKDATA, pid, (void *)pc, NULL);
    if (errno) {
        ERR("Cannot peek at child code - %s", strerror(errno));
        return FAILURE;
    }

    /* restore original instruction, step over it */
    if (-1 == ptrace(PTRACE_POKEDATA, pid, (void *)pc, (void *)((instr & ~0xFF) | saved))) {
        ERR("Cannot update child code - %s", strerror(errno));
        return FAILURE;
    }
    set_ip(pid, pc);
    if (-1 == ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL)) {
        ERR("Cannot restore original instruction - %s", strerror(errno));
        return FAILURE;
    }

    int wait_status;
    waitpid(pid, &wait_status, 0);      // wait for SIGTRAP from child, indicating the breakpoint
    if (!WIFSTOPPED(wait_status) || !(WSTOPSIG(wait_status) == SIGTRAP)) {
        ERR("Didn't get expected SIGTRAP - got %d", WSTOPSIG(wait_status));
        return FAILURE;
    }

    /* re-instate TRAP */
    if (-1 == ptrace(PTRACE_POKEDATA, pid, (void *)pc, (void *)instr)) {
        ERR("Cannot update child code - %s", strerror(errno));
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   set_ip
 *
 *  Params:     pid - pid of process being traced
 *              ip  - new value of IP register
 *
 *  Return:     N/A
 *
 *  Descr:      Set process's IP
 *
 **************************************************************************/
void set_ip(pid_t pid, REG_TYPE ip) {
    struct user_regs_struct regs;
    if (-1 == ptrace(PTRACE_GETREGS, pid, NULL, &regs)) {
        printf("Cannot read process registers - %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    IP(regs) = ip;
    if (-1 == ptrace(PTRACE_SETREGS, pid, NULL, &regs)) {
        printf("Cannot set process registers - %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}


/**************************************************************************
 *
 *  Function:   bpf_callback
 *
 *  Params:     unused
 *              data - message
 *              data_size - message size
 *
 *  Return:     N/A
 *
 *  Descr:      Process messages form BPF programs
 *
 **************************************************************************/
void bpf_callback(void *unused, void *data, int data_size) {
    (void)unused;
    (void)data_size;
    /* I assume that MMAPENTRY is always followed by MMAPEXIT, so saving size from ENTRY
       to use it when got EXIT */
    static uint64_t mapped_size = 0;
    static int locked = 0;

    struct bpf_event *event = data;
    if (BPF_EVT_SIGNAL == event->type) {
        INFO("signal %" PRId64, event->payload);
        if (locked) {
            INFO("unlocking");
            int ret = pthread_mutex_unlock(&cachedmem_access);
            if (ret) {
                ERR("Error unlocking mutex: %s", strerror(ret));
                return;
            }
            locked = 0;
        }
        return;
    } else if (!locked) {
        INFO("locking");
        int ret = pthread_mutex_lock(&cachedmem_access);
        if (ret) {
            ERR("Error locking mutex: %s", strerror(ret));
            return;
        }
        locked = 1;
    }
    
    switch (event->type) {
        case BPF_EVT_PAGEFAULT:
            INFO("Page fault at %" PRIx64, event->payload);
            mark_dirty(event->payload);
            break;
        case BPF_EVT_MMAPENTRY:
            mapped_size = event->payload;
            break;
        case BPF_EVT_MMAPEXIT:
            INFO("New map at %" PRIx64 " for %" PRId64, event->payload, mapped_size);
            cache_add_region(event->payload, mapped_size);
            break;
        case BPF_EVT_MUNMAP:
            INFO("Unmap at %" PRIx64, event->payload);
            break;
        default:
            WARN("Inknown event type %" PRId64 "\n", event->type);
    }
}
