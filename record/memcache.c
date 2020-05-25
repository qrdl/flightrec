/**************************************************************************
 *
 *  File:       memcache.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Memory cache initialisation, maintenace and processing
 *
 *  Notes:
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
#include <stdio.h>
#include <stdlib.h>
#include <stdalign.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <linux/limits.h>
#include <pthread.h>

#include "flightrec.h"
#include "record.h"
#include "eel.h"
#include "mem.h"
#include "memcache.h"
#include "db_workers.h"
#include "channel.h"

/* memory region, initially corresponds to single entry in /proc/<pid>/maps, but later if region grows,
   added chunk is processed as separate region */
struct region {
    uint64_t        start;
    uint64_t        end;
    char            *pages;
};

static uint64_t find_page(uint64_t address, char **cached);
static void process_page(uint64_t address, char *cached, uint64_t step_id);

/* sorted array of memory regions */
static struct region *cache;
static unsigned int reg_count;

static pid_t child_pid;
extern struct channel *proc_mem_ch;

/* function pointers for best memory comparison functions, based on available CPU features and size */
int (* memdiff)(const char *buf1, const char *buf2, size_t count);

/**************************************************************************
 *
 *  Function:   init_cache
 *
 *  Params:     pid
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Read initial memory map of the process, initialise the
 *              cache
 *
 **************************************************************************/
int init_cache(pid_t pid) {
    char exe_name[PATH_MAX];
    char tmp[256];

    memdiff = best_memdiff(MEM_SEGMENT_SIZE);
    child_pid = pid;

    snprintf(tmp, sizeof(tmp), "/proc/%d/exe", pid);
    ssize_t res = readlink(tmp, exe_name, sizeof(exe_name) - 1);
    if (res < 0) {
        ERR("Cannot get executable name from '%s': %s", tmp, strerror(errno));
        return FAILURE;
    }
    exe_name[res] = '\0';

    snprintf(tmp, sizeof(tmp), "/proc/%d/maps", pid);
    FILE *maps = fopen(tmp, "r");
    if (!maps) {
        ERR("Cannot open file '%s': %s", tmp, strerror(errno));
        return FAILURE;
    }

    while (fgets(tmp, sizeof(tmp), maps)) {
        char *state, *field;
        field = strtok_r(tmp, " \t", &state);
        if (!field) {
            continue;
        }
        field = strtok_r(NULL, " \t", &state);
        if (!field || 'r' != field[0]) {     // skip memory without read permissions
            continue;
        }
        int i = 2;  // two fields processed already
        while (i < 6 && NULL != (field = strtok_r(NULL, " \t\n", &state))) {
            i++;
        }
        if (    !field ||       // no filename specified - can be heap
                // last field (space-separated) contains the region name/mapped file name
                !strcmp(field, "[heap]") ||
                !strcmp(field, "[stack]") ||
                !strcmp(field, exe_name)        // mapped exe pages - globals and statics
                ) {

            /* add memory region to cache */
            uint64_t head, tail;
            /* first field, already 0-terminated, has start and end address */
            if (2 != sscanf(tmp, "%" PRIx64 "-%" PRIx64, &head, &tail)) {
                WARN("Cannot read memory regions");
                continue;
            }
            cache_add_region(head, tail-head, 1);       // add new memory for first step
        }
    }
    fclose(maps);

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   find_page
 *
 *  Params:     address - memory address
 *              cached - where to store pointer to cached memory page
 *
 *  Return:     start address of memory page
 *
 *  Descr:      Find page by address
 *
 **************************************************************************/
uint64_t find_page(uint64_t address, char **cached) {
    int index;
    /* use binary search to find region the address belongs to */
    /* I don't want to use bsearch() to avoid extra function calls */
    int left = 0;
    int right = reg_count;
    for (index = right / 2; right > left; index = (left + right) / 2) {
        if (address < cache[index].start) {
            right = index;
        } else if (address >= cache[index].end) {
            left = index + 1;
        } else {
            break;
        }
    }
    if (left == right) {
        DBG("Address 0x%" PRIx64 " not found in cache", address);
        return 0;
    }
    int page_num = (address - cache[index].start) / PAGE_SIZE;
    *cached  = cache[index].pages + page_num * PAGE_SIZE;
    return cache[index].start + page_num * PAGE_SIZE;
}


/**************************************************************************
 *
 *  Function:   process_page
 *
 *  Params:     address - page address (in child memory space)
 *              cached - pointer to cached page content
 *              step_id
 *
 *  Return:     N/A
 *
 *  Descr:      Find changed part of the page, store the changes into DB
 *              (by calling worker), cache new content
 *
 **************************************************************************/
void process_page(uint64_t address, char *cached, uint64_t step_id) {
    /* buffer must be aligned to allow fast vector instructions */
    alignas(MEM_SEGMENT_SIZE) char buffer[PAGE_SIZE];
    struct iovec local = {buffer, PAGE_SIZE};
    struct iovec child = {(void *)address, PAGE_SIZE};
    if (process_vm_readv(child_pid, &local, 1, &child, 1, 0) < (ssize_t)MEM_SEGMENT_SIZE) {
        ERR("Cannot read child memory: %s", strerror(errno));
        return;
    }

    /* loop through page segments, look for changed one */
    for (uint64_t offset = 0; offset < PAGE_SIZE; offset += MEM_SEGMENT_SIZE) {
        if (memdiff(buffer + offset, cached + offset, MEM_SEGMENT_SIZE)) {
            // found changed segment
            memcpy(cached + offset, buffer + offset, MEM_SEGMENT_SIZE);

            /* store memory change event in DB using workier */
            struct insert_mem_msg *msg = malloc(sizeof(*msg));
            msg->address = address + offset;
            msg->step_id = step_id;
            memcpy(msg->content, buffer+offset, MEM_SEGMENT_SIZE);
            ch_write(insert_mem_ch, (char *)msg, sizeof(*msg));    // channel reader will free msg
        }
    }
}


/**************************************************************************
 *
 *  Function:   cache_add_region
 *
 *  Params:     address - start adress of new region
 *              size
 *
 *  Return:     N/A
 *
 *  Descr:      Add new memory region to cache, memory regions are stored
 *              in arrey, sorted by start address to speed up address lookup
 *
 **************************************************************************/
void cache_add_region(uint64_t address, uint64_t size, uint64_t step_id) {
    /* look for potential position of new region in the cache */
    unsigned int index;
    for (index = 0; index < reg_count && cache[index].start <= address; index++);

    struct region *tmp = cache;
    reg_count++;
    cache = malloc(sizeof(*cache) * reg_count);
    struct region *new_reg = NULL;
    if (!tmp) {
        /* first region */
        new_reg = cache;
    } else {
        if (index > 0) {
            /* copy regions preceeding the new one */
            memcpy(cache, tmp, sizeof(*cache) * index);
        }
        if (index < (reg_count-1)) {
            /* copy regions following the new one */
            DBG("Copy to %d from %d %lu bytes", index + 1, index, sizeof(*cache) * (reg_count - index - 1));
            memcpy(&cache[index + 1], &tmp[index], sizeof(*cache) * (reg_count - index - 1));
        }
        new_reg = cache + index;
        free(tmp);
    }
    new_reg->start = address;
    new_reg->end = address + size;

    /* it is important to use aligned momory as CPU instructions used by memdiff operates with aligned memory */
    posix_memalign((void **)&new_reg->pages, MEM_SEGMENT_SIZE, size);
    struct iovec local = {new_reg->pages, size};
    struct iovec child = {(void *)address, size};
    if (process_vm_readv(child_pid, &local, 1, &child, 1, 0) < (ssize_t)size) {
        ERR("Cannot read child memory: %s", strerror(errno));
        return;
    }
    for (uint64_t offset = 0; offset < size; offset += MEM_SEGMENT_SIZE) {
        /* store memory change event in DB using workier */
        struct insert_mem_msg *msg = malloc(sizeof(*msg));
        msg->address = address + offset;
        msg->step_id = step_id;
        memcpy(msg->content, new_reg->pages + offset, MEM_SEGMENT_SIZE);
        ch_write(insert_mem_ch, (char *)msg, sizeof(*msg));    // channel reader will free msg
    }

    INFO("Added mem region at 0x%" PRIx64 " for %" PRId64, address, size);
}


/**************************************************************************
 *
 *  Function:   proc_dirty_mem
 *
 *  Params:     step_id
  *
 *  Return:     N/A
 *
 *  Descr:      Process page fault events and process dirty pages
 *
 **************************************************************************/
void proc_dirty_mem(uint64_t step_id) {
    uint64_t *address;
    uint64_t page_address;
    char *cached;
    size_t size = sizeof(*address);
    /* read and process until there is something to process */
    while (CHANNEL_OK == ch_read(proc_mem_ch, (char **)&address, &size, READ_NONBLOCK)) {
        DBG("Dirty addr 0x%" PRIx64 " at step %" PRId64, *address, step_id);
        page_address = find_page(*address, &cached);
        if (page_address) {
            process_page(page_address, cached, step_id);
        }

        free(address);
    }
    return;
}
