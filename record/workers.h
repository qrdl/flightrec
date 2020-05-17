/**************************************************************************
 *
 *  File:       workers.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Worker threads for writing/updating DB
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
#ifndef _WORKERS_H
#define _WORKERS_H

#include <sys/user.h>   // for struct user_regs_struct

#include "flightrec.h"
#include "stingray.h"
#include "mem.h"        // for MEM_SEGMENT_SIZE

// cannot wrap into do {...} while(0) because have to declare some variables
#define START_WORKER(A) \
        insert_ ## A ## _ch = ch_create(); \
        if (!insert_ ## A ## _ch) { \
            return FAILURE; \
        } \
        pthread_t insert_ ## A ## _worker; \
        if (0 != pthread_create(&insert_ ## A ## _worker, NULL, wrk_insert_ ## A, insert_ ## A ## _ch)) { \
            ERR("Cannot start insert " #A " worker thread: %s", strerror(errno)); \
            return FAILURE; \
        }
#define WAIT_WORKER(A) do { \
        ch_finish(insert_ ## A ## _ch); \
        void *res; \
        if (0 != pthread_join(insert_ ## A ## _worker, &res)) { \
            ERR("Cannot join insert " #A " worker thread: %s", strerror(errno)); \
            return FAILURE; \
        } \
        if (!res) { \
            ERR("insert " #A " worker failed"); \
            return FAILURE; \
        } \
        ch_destroy(insert_ ## A ## _ch); \
    } while (0)

struct insert_step_msg {
    ULONG                       step_id;
    ULONG                       depth;
    ULONG                       func_id;
    ULONG                       address;
    struct user_regs_struct     regs;
};

struct insert_heap_msg {
    ULONG   step_id;
    ULONG   address;
    ULONG   size;
};

struct insert_mem_msg {
    ULONG   step_id;
    ULONG   address;
    char    content[MEM_SEGMENT_SIZE];
};

void *wrk_insert_step(void *arg);
void *wrk_insert_heap(void *arg);
void *wrk_insert_mem(void *arg);

extern struct channel *insert_mem_ch;      // defined in run.c

#endif
