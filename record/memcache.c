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
#include <stdalign.h>
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
static FILE *clear_refs;

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
            cache_add_region(head, tail-head);
        }
    }
    fclose(maps);

    snprintf(tmp, sizeof(tmp), "/proc/%d/clear_refs", pid);
    clear_refs = fopen(tmp, "w");
    if (!clear_refs) {
        ERR("Cannot open file '%s': %s", tmp, strerror(errno));
        return FAILURE;
    }

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
        DBG("Address %lx not found in cache", address);
        return;
    }
    int page_num = (address - cache[index].start) / PAGE_SIZE;
    DBG("Marking page %d in region starting %" PRIx64, page_num, cache[index].start);
    char mask = 0x80 >> (page_num % 8);
    cache[index].bitmap[page_num / 8] |= mask;
    cache[index].flags = FLAG_DIRTY;
}


#define CHECK_PAGE(I) if (_8pages & (0x80 >> (I))) { \
    page_offset = PAGE_SIZE * (page_index + (I)); \
    DBG("Dirty page %d in region starting %" PRIx64, page_index + (I), cache[i].start); \
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
    unsigned int i, j, k;
    unsigned int page_index;
    uint64_t page_offset;

    for (i = 0; i < reg_count; i++) {
        if (cache[i].flags & FLAG_DIRTY) {      // some pages in the region are dirty
            div_t res = div(cache[i].page_count, 8);
            unsigned int slots = res.quot + (res.rem ? 1 : 0);     // each slot is 1 byte representing 8 pages
            for (j = 0; j < slots; j += sizeof(uint64_t)) {
                uint64_t _64pages = *(uint64_t *)(cache[i].bitmap + j);
                DBG("0x%" PRIx64 " %d-%d: 0x%" PRIx64, cache[i].start, j, j + 64, _64pages);
                if (_64pages) {                 // one of 64 checked pages dirty
                    for (k = 0; k < 8; k++) {
                        char _8pages = cache[i].bitmap[j+k];
                        if (_8pages) {          // one of 8 pages dirty
                            page_index = (j+k) * 8;     // page number from bit position
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
            memset(cache[i].bitmap, 0, slots);  // clear page bitmap
        }
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   reset_dirty
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Reset system dirty flag for all memory pages to force page
 *              faults on any change
 *
 **************************************************************************/
void reset_dirty(void) {
    static char buf = '4';     // '4' resets soft-dirty bits

    rewind(clear_refs);
    if (!fwrite(&buf, 1, 1, clear_refs)) {
        ERR("Cannot write to clear_refs file");
        return;
    }
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
    new_reg->page_count = size / PAGE_SIZE;     // size if always divisible by PAGE_SIZE because mem allocated in pages
    new_reg->flags = FLAG_DIRTY;
    /* bitmap must be divisible by 8 to speed up processing */
    new_reg->bitmap = calloc(new_reg->page_count / 64 + (new_reg->page_count % 64 ? 1 : 0), sizeof(uint64_t));     // allocate enough 8-byte chunks, each for 64 pages
    /* Set 1 for existing pages (all padding bits should be 0s) to force caching of new pages */
    div_t res = div(new_reg->page_count, 8);
    memset(new_reg->bitmap, 0xFF, res.quot);
    if (res.rem) {
        int mask = 0;
        for (int i = 0; i < res.rem; i++) {
            mask |= 0x80 >> i;
        }
        new_reg->bitmap[res.quot] = mask;
    }

    /* it is important to use aligned momory as CPU instructions used by memdiff operates with aligned memory */
    posix_memalign((void **)&new_reg->pages, MEM_SEGMENT_SIZE, size);
    memset(new_reg->pages, 0, size);
    INFO("Added mem region at %" PRIx64 " for %" PRId64, address, size);
}

void cache_status(void) {
    for (unsigned int i = 0; i < reg_count; i++) {
        DBG("Reg %" PRIx64 " for %" PRId64, cache[i].start, cache[i].end - cache[i].start);
    }
}
