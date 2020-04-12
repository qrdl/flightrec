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
#include "channel.h"
#include "workers.h"

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

unsigned long page_size;
int fifo_fd = 0;

static struct channel *insert_step_ch;
static struct channel *insert_heap_ch;
struct channel *insert_mem_ch;          // not static because is used in mem.c
/* child program base address. Addresses from debug info may or may not contain base address, so if accessing child
   memory fails, assume addresses need to be adjusted by base address */
static uint64_t base_address = 0;


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

        int first = 1;
        int signum;
        while (-1 != ptrace(PTRACE_CONT, pid, NULL, NULL)) {
            waitpid(pid, &wait_status, 0);      // wait for SIGTRAP from child, indicating the breakpoint
            if (WIFEXITED(wait_status)) {
                INFO("child exited");
                signum = 0;
                break;                          // child exited ok
            }
            if (first) {
                /* cannot init memory before as this is the first point where tracee is
                   fully initialised.
                   TODO Consider moving out of loop */
                if (SUCCESS != mem_init(pid)) {
                    return FAILURE;
                }
                first = 0;
            }

            if (WIFSTOPPED(wait_status)) {
                signum = WSTOPSIG(wait_status);
                if (SIGTRAP == signum) {
                    if (SUCCESS != process_breakpoint(pid)) {
                        return FAILURE;
                    }
                } else {
                    INFO("child stopped - sig %d", signum);
                    break;                          // child exited ok
                }
            } else {
                // TODO: can we get here?
                ERR("Unsupported wait status %d", wait_status);
                return FAILURE;
            }
        }

        INFO("Waiting for worker threads to finish");

        /* Send termination to workers and wait for workers to finish. Because workers
           flush data to DB, process them one by one */
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
                return NULL;
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

    // TODO Change update by PK to update by rowid - simpler and probably faster
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
                if (EIO == errno && !base_address) {
                    /* may fail because address needs to be adjusted by base address */
                    if (SUCCESS != get_base_address(pid, &base_address)) {
                        RETCLEAN(FAILURE);
                    }
                    if (!base_address) {
                        ERR("Cannot peek at child code (base addr is zero) - %s", strerror(errno));
                        RETCLEAN(FAILURE);
                    }
                    /* adjust unit addesses in DB */
                    if (DAB_OK != DAB_EXEC("UPDATE unit SET base_addr = base_addr + ?", base_address)) {
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

    if (func_id != old_func_id) {
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
//            DBG("Malloc for %" PRIu64 " bytes", event.size);
            // if new address doesn't fall into known ranges, re-read memory map
            if (!mem_in_cache(event.address, event.size)) {
                if (SUCCESS != mem_read_regions()) {
                    return FAILURE;
                }
            }
            msg->size = event.size;
        } else {
//            DBG("Free");
            msg->size = 0;      // indicate 'free'
        }
        ch_write(insert_heap_ch, (char *)msg, sizeof(*msg));    // channel reader will free msg
    }
    if (read_status < 0 && errno != EAGAIN) {
        ERR("Error reading from pipe: %s", strerror(errno));
        return FAILURE;
    }

    if (FUNC_FLAG_START != func_flag) {
        uint64_t start, end;
        int ret;
        int dirty_found = 0;
        /* loop through memory regions of interest (such as heap and stack), process "dirty" pages */
        for (ret = mem_first_region(&start, &end); SUCCESS == ret; ret = mem_next_region(&start, &end)) {
            ret = mem_process_region(start, end, step_id, 0);
            if (FOUND == ret) {
                dirty_found = 1;
            } else if (SUCCESS != ret) {
                return FAILURE;
            }
        }

        if (FUNC_FLAG_END == func_flag) {
            depth--;
        }
        if (dirty_found) {
            if (SUCCESS != mem_reset_dirty()) {
                return FAILURE;
            }
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

