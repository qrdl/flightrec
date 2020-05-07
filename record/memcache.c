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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <linux/limits.h>

#include "flightrec.h"
#include "eel.h"
#include "mem.h"
#include "memcache.h"
#include "workers.h"
#include "channel.h"

#define FLAG_DIRTY  1

/* memory region, initially corresponds to single entry in /proc/<pid>/maps, but later if region grows,
   added chunk is processed as separate region */
struct region {
    uint64_t        start;
    uint64_t        end;
    uint64_t        page_count;
    uint64_t        flags;
    char            *bitmap;        // bitmap size depends on page_count, one bit per page
    char            *pages;
};

static void process_page(uint64_t address, char *cached, uint64_t step);

/* sorted array of memory regions */
static struct region *cache;
static unsigned int reg_count;

static pid_t child_pid;
static char *clear_refs_filename;

/* function pointers for best memory comparison functions, based on available CPU features and size */
int (* memdiff)(const char *buf1, const char *buf2, size_t count);
uint32_t (* memisset)(const char *buf, size_t count);

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
    memisset = best_memisset(MEM_SEGMENT_SIZE);
    child_pid = pid;

    if (!*exe_name) {   // read executable name from /proc/<pid>/exe - executed once
        snprintf(tmp, sizeof(tmp), "/proc/%d/exe", pid);
        ssize_t res = readlink(tmp, exe_name, sizeof(exe_name) - 1);
        if (res < 0) {
            ERR("Cannot get executable name from '%s': %s", tmp, strerror(errno));
            return FAILURE;
        }
        exe_name[res] = '\0';
    }

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
            cache_add_region(head, tail-head);
        }
    }
    fclose(maps);

    snprintf(tmp, sizeof(tmp), "/proc/%d/clear_refs", pid);
    clear_refs_filename = strdup(tmp);

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   mark_dirty
 *
 *  Params:     address
 *
 *  Return:     N/A
 *
 *  Descr:      Find address in cache, mark containing page and region as
 *              dirty
 *
 **************************************************************************/
void mark_dirty(uint64_t address) {
    int index;
    /* use binary search to find region the address belongs to */
    /* I don't want to use bsearch() to avoid extra function calls */
    int left = 0;
    int right = reg_count;
    for (index = reg_count / 2; right > left; index = (left + right) / 2) {
        if (address < cache[index].start) {
            right = index;
        } else if (address > cache[index].end) {
            left = index;
        } else {
            break;
        }
    }
    if (right == left) {
        WARN("Address %lx not found in cache", address);
        return;
    }
    int page_num = (address - cache[index].start) / PAGE_SIZE;
    char mask = 1 << (page_num % 8);
    cache[index].bitmap[page_num / 8] |= mask;
    cache[index].flags = FLAG_DIRTY;
}


#define CHECK_PAGE(I) if (eight_pages & (1 << (I))) { \
    page_offset = PAGE_SIZE * (page_index + (I)); \
    process_page(cache[i].start + page_offset, cache[i].pages + page_offset, step); \
}
/**************************************************************************
 *
 *  Function:   process_dirty
 *
 *  Params:     step - step ID to associate memory changes to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Loop through all regions, for dirty regions identify dirty
 *              pages, compare pages with cached ones, store difference in
 *              DB using worker thread, update cache.
 *              After processing reset dirty flag.
 *              There is a potential for speed-up by processing regions
 *              in worker threads, because every region is well-isolated
 *
 **************************************************************************/
int process_dirty(uint64_t step) {
    uint32_t dirty_pages;
    unsigned int i, j, k;
    unsigned int page_index;
    char eight_pages;
    uint64_t page_offset;

    for (i = 0; i < reg_count; i++) {
        if (cache[i].flags & FLAG_DIRTY) {
            for (j = 0; j < cache[i].page_count; j += MEM_SEGMENT_SIZE) {
                /* check MEM_SEGMENT_SIZE*8 number of pages */
                dirty_pages = memisset(cache[i].bitmap + j, MEM_SEGMENT_SIZE);
                if (dirty_pages) {
                    /* each bit of dirty_pages means 8 pages */
                    for (k = 0; k < sizeof(uint32_t) * 8 ; k++) {
                        if (dirty_pages & (1 << k)) {
                            page_index = j + k*8;
                            eight_pages = cache[i].bitmap[page_index];
                            /* unrolled loop */
                            CHECK_PAGE(0);
                            CHECK_PAGE(1);
                            CHECK_PAGE(2);
                            CHECK_PAGE(3);
                            CHECK_PAGE(4);
                            CHECK_PAGE(5);
                            CHECK_PAGE(6);
                            CHECK_PAGE(7);
                        }
                    }
                }
            }
            cache[i].flags = 0;                 // clear region dirty flag
            memset(cache[i].bitmap, 0, j / 8);  // clear page bitmap
        }
    }

    /* reset system dirty flag for all memory pages to force page faults on any change */
    // TODO Can I keep the file opened to avoid opening/closing every time?
    FILE *clear_refs = fopen(clear_refs_filename, "w");
    if (!clear_refs) {
        ERR("Cannot open file '%s': %s", clear_refs_filename, strerror(errno));
        return FAILURE;
    }
    char buf = '4';     // '4' resets soft-dirty bits
    if (!fwrite(&buf, 1, 1, clear_refs)) {
        ERR("Cannot write to file '%s'", clear_refs_filename);
        fclose(clear_refs);
        return FAILURE;
    }
    fclose(clear_refs);

    return SUCCESS;
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
    char buffer[MEM_SEGMENT_SIZE];
    struct iovec local = {buffer, MEM_SEGMENT_SIZE};
    struct iovec child = {(void *)address, MEM_SEGMENT_SIZE};

    uint64_t page_end = address + PAGE_SIZE;
    /* loop through page segments, look for changed one */
    for (uint64_t cur = address; cur < page_end; cur += MEM_SEGMENT_SIZE) {
        if (process_vm_readv(child_pid, &local, 1, &child, 1, 0) < (ssize_t)MEM_SEGMENT_SIZE) {
            ERR("Cannot read child memory: %s", strerror(errno));
            return;
        }
        if (memdiff(buffer, cached, MEM_SEGMENT_SIZE)) {    // found changed segment
            memcpy(cached, buffer, MEM_SEGMENT_SIZE);

            /* store memory change event in DB using workier */
            struct insert_mem_msg *msg = malloc(sizeof(*msg));
            msg->address = address;
            msg->step_id = step_id;
            memcpy(msg->content, buffer, MEM_SEGMENT_SIZE);
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
void cache_add_region(uint64_t address, uint64_t size) {
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
        if (index < reg_count) {
            /* copy regions following the new one */
            memcpy(cache + sizeof(*cache) * (index + 1), tmp + sizeof(*cache) * index, sizeof(*cache) * (reg_count - index));
        }
        new_reg = cache + index;
        free(tmp);
    }
    new_reg->start = address;
    new_reg->end = address + size;
    new_reg->page_count = size / PAGE_SIZE;     // size if always divisible by PAGE_SIZE because mem allocated in pages
    new_reg->flags = 0;
    int bitmap_size = new_reg->page_count / 8;
    int remainder = new_reg->page_count % 8;
    if (remainder) {
        bitmap_size++;  // page count isn't divisible by 8, add one extra byte
    }
    if (bitmap_size % MEM_SEGMENT_SIZE) {
        /* add padding to use vector instructions - bitmap size must be divisible by MEM_SEGMENT_SIZE */
        bitmap_size += MEM_SEGMENT_SIZE - bitmap_size % MEM_SEGMENT_SIZE;
    }
    /* it is important to use aligned momory as CPU instructions used by memdiff operates with aligned memory */
    posix_memalign((void **)&new_reg->pages, MEM_SEGMENT_SIZE, size);
    memset(new_reg->pages, 0, size);
    posix_memalign((void **)&new_reg->bitmap, MEM_SEGMENT_SIZE, bitmap_size);
    /* Set 1 for existing pages (all padding bits should be 0s) to force caching of new pages */
    memset(new_reg->bitmap, 0xFF, new_reg->page_count / 8);
    if (remainder) {
        int mask = 0;
        for (int i = 0; i < remainder; i++) {
            mask = mask < 1 + 1;
        }
        new_reg->bitmap[new_reg->page_count / 8] = mask;
    }
}
