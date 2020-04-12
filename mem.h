/**************************************************************************
 *
 *  File:       mem.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Common definitions
 *
 *  Notes:      Memeory-related definitions and functions, used mostly by
 *              'record' component
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
#ifndef _MEM_H
#define _MEM_H

#include <inttypes.h>
#include <sys/types.h>

// 32 is optimal for AVX2 instruction set - requires just one instruction to compare buffers
#define MEM_SEGMENT_SIZE    32

#define HEAP_EVENT_ALLOC    1
#define HEAP_EVENT_FREE     2

/* memory allocation/deallocation events, sent by preload shared lib to tracer */
struct heap_event {
    int         type;
    uint64_t    address;
    uint64_t    size;
};

/* this struct is used for passing args to mem_process_region() */
struct region_proc_args {
    uint64_t    start;
    uint64_t    end;
    uint64_t    step_id;
};

/* child process memory operations */
int mem_init(pid_t pid);
int mem_read_regions(void);
int mem_first_region(uint64_t *start, uint64_t *end);
int mem_next_region(uint64_t *start, uint64_t *end);
int mem_process_region(uint64_t start, uint64_t end, uint64_t step_id, int force);
int mem_in_cache(uint64_t address, uint64_t size);
int mem_reset_dirty(void);
int get_base_address(pid_t p, uint64_t *offset);

/* function returns pointer to function with fastest implementation based on instruction set and buffer size */
int (* best_memdiff(size_t count))(const char *, const char *, size_t);

#endif

