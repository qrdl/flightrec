/**************************************************************************
 *
 *  File:       mem.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Memory checking
 *
 *  Notes:      Functions in this unit are used for checking if tracee's
 *              memory changed, and record any found changes
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
#include <inttypes.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <linux/limits.h>
#include <stdlib.h>
#ifdef __AVX__
#include <x86intrin.h>
#endif

#include "eel.h"
#include "stingray.h"
#include "dab.h"

#include "flightrec.h"
#include "record.h"
#include "mem.h"
#include "channel.h"
#include "workers.h"

/* TODO: consider using array rather than linked list for cache locality */
struct region {
    uint64_t        start;
    uint64_t        end;
    struct region   *next;
    char            **pages;
};

static int process_dirty_page(ULONG start, ULONG step_id);
static char *mapped_mem(uint64_t address);
static const char *mem_current(uint64_t address, uint64_t size);
#ifdef __AVX__
static __m256i mask;
#endif

static struct region *regions, *cur_region;
static FILE *pagemap = NULL;
static pid_t pid;
static char *clear_refs_filename;
#ifdef __AVX__
static int avx_supported = 0;
#endif

int (* memdiff)(const char *buf1, const char *buf2, size_t count);

extern struct channel *insert_mem_ch;      // defined in run.c

/**************************************************************************
 *
 *  Function:   open_pagemap
 *
 *  Params:     pid
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Open pagemap file for firther use
 *
 **************************************************************************/
int mem_init(pid_t p) {
    pid = p;
    char tmp[256];

    if (!pagemap) {
        snprintf(tmp, sizeof(tmp), "/proc/%d/pagemap", pid);
        pagemap = fopen(tmp, "rb");  // this is binary file
        if (!pagemap) {
            ERR("Cannot open file '%s': %s", tmp, strerror(errno));
            return FAILURE;
        }
    }

    snprintf(tmp, sizeof(tmp), "/proc/%d/clear_refs", pid);
    clear_refs_filename = strdup(tmp);

    memdiff = best_memdiff(MEM_SEGMENT_SIZE);    // get best memdiff implementtion based on available CPU features

    if (SUCCESS != mem_read_regions()) {
        return FAILURE;
    }

    /* loop through memory regions of interest (such as heap and stack), store initial memory content */
    uint64_t start, end;
    for (int ret = mem_first_region(&start, &end); SUCCESS == ret; ret = mem_next_region(&start, &end)) {
        if (FAILURE == mem_process_region(start, end, 0, 1)) {
            return FAILURE;
        }
    }

    if (SUCCESS != mem_reset_dirty()) {
        return FAILURE;
    }

#ifdef __AVX__
    /* initialise vector mask to check 4 memory pages with one instruction */
    if (__builtin_cpu_supports("avx")) {
        avx_supported = 1;
        const char tmp[] = {    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00};

        mask = _mm256_loadu_si256((__m256i const *)tmp);
    }
#endif

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   mem_first_region
 *
 *  Params:     pid
 *              start - where to store region start
 *              end - where to store region end
 *
 *  Return:     SUCCESS / FAILURE / END
 *
 *  Descr:      Find memory region boundaries by parsing /proc/<pid>/maps
 *
 **************************************************************************/
int mem_first_region(uint64_t *start, uint64_t *end) {
    if (!regions) {
        cur_region = NULL;
        return END;
    }

    *start = regions->start;
    *end = regions->end;
    cur_region = regions;      // TODO cur is used to store the state for mem_next_region, not thread-safe

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   mem_next_region
 *
 *  Params:     start - where to store region start
 *              end - where to store region end
 *
 *  Return:     SUCCESS / FAILURE / END
 *
 *  Descr:      return next regions from linked list
 *
 **************************************************************************/
int mem_next_region(uint64_t *start, uint64_t *end) {
    if (!cur_region) {
        return FAILURE;
    }
    if (!cur_region->next) {
        return END;
    }

    cur_region = cur_region->next;
    *start = cur_region->start;
    *end = cur_region->end;

    return SUCCESS;
}


#ifdef __AVX__
/**************************************************************************
 *
 *  Function:   pages_clean
 *
 *  Params:     buf - 32-byte buffer to check (4 page descriptors)
 *
 *  Return:     1 (all pages clean) / 0 (some pages dirty)
 *
 *  Descr:      look for dirty bits in page descriptors by checking four
 *              8-byte blocks for 55th bit (15th in little-endian system)
 *              set in every 64 bits
 *
 **************************************************************************/
static inline int pages_clean(const char *buf) {
    return _mm256_testz_si256(_mm256_loadu_si256((__m256i const *)buf), mask);
}
#endif

#define PAGE_DESCR_SIZE     8
// for little-endian system soft-dirty bit (55th bit of 64) is 15th bit, or 7th bit of 6th byte
#define DIRTY_OFFSET        6
#define DIRTY_BIT           (1 << 7)
/**************************************************************************
 *
 *  Function:   mem_process_region
 *
 *  Params:     start - region start address
 *              end -   region end address, don't look for pages beyond this
 *                      address
 *
 *  Return:     SUCCESS / FAILURE / FOUND
 *
 *  Descr:      Process all dirty pages within memory region by parsing
 *              /proc/pid/pagemap
 *              See "man 5 proc" and Linux kernel docs at
 *              Documentation/admin-guide/mm/soft-dirty.rst
 *
 **************************************************************************/
int mem_process_region(ULONG start, ULONG end, ULONG step_id, int force) {
    uint64_t offset = start / PAGE_SIZE * sizeof(uint64_t);    // each page table entry is 64 bit
    ULONG page_count = (end - start) / PAGE_SIZE;
    char *entry = malloc(page_count * PAGE_DESCR_SIZE);

    if (force) {
        /* to avoid excessive comparisons within the loop for the first run manually mark all pages dirty */
        memset(entry, DIRTY_BIT, page_count * PAGE_DESCR_SIZE);
    } else {
        if (fseek(pagemap, offset, SEEK_SET)) {
            ERR("Cannot position to page entry in '%s': %s", tmp, strerror(errno));
            free(entry);
            return FAILURE;
        }
        if (page_count != fread(entry, PAGE_DESCR_SIZE, page_count, pagemap)) {
            ERR("Cannot read page entry from '%s'", tmp);
            free(entry);
            return FAILURE;
        }
    }

    int dirty_found = 0;

#ifdef __AVX__
    if (avx_supported) {
        char *cur = entry;
        ULONG page_num;
        // AVX operates with 256 bit vectors so every vector contains 4 page descriptions
        for (page_num = 0; page_count - page_num >= 4; page_num += 4) {
            if (!pages_clean(cur)) {
                /* at least one of 4 tested pages is dirty */
                dirty_found = 1;
                if (cur[DIRTY_OFFSET] & DIRTY_BIT) {
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*page_num, step_id)) {
                        return FAILURE;
                    }
                }
                if (cur[DIRTY_OFFSET + PAGE_DESCR_SIZE] & DIRTY_BIT) {
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*(page_num+1), step_id)) {
                        return FAILURE;
                    }
                }
                if (cur[DIRTY_OFFSET + PAGE_DESCR_SIZE*2] & DIRTY_BIT) {
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*(page_num+2), step_id)) {
                        return FAILURE;
                    }
                }
                if (cur[DIRTY_OFFSET + PAGE_DESCR_SIZE*3] & DIRTY_BIT) {
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*(page_num+3), step_id)) {
                        return FAILURE;
                    }
                }
            }
            cur += PAGE_DESCR_SIZE * 4;
        }
        cur += DIRTY_OFFSET;    // shift to required bit

        switch (page_count - page_num) {
            case 3:
                if (*cur & DIRTY_BIT) {
                    // found page with set soft-dirty bit
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*page_num, step_id)) {
                        return FAILURE;
                    }
                    dirty_found = 1;
                }
                page_num++;
                cur += PAGE_DESCR_SIZE;
                /* FALLTHROUGH */
            case 2:
                if (*cur & DIRTY_BIT) {
                    // found page with set soft-dirty bit
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*page_num, step_id)) {
                        return FAILURE;
                    }
                    dirty_found = 1;
                }
                page_num++;
                cur += PAGE_DESCR_SIZE;
                /* FALLTHROUGH */
            case 1:
                if (*cur & DIRTY_BIT) {
                    // found page with set soft-dirty bit
                    if (SUCCESS != process_dirty_page(start + PAGE_SIZE*page_num, step_id)) {
                        return FAILURE;
                    }
                    dirty_found = 1;
                }
        }
    } else
#endif
    {
        // fallback to slow comparison
        char *cur = entry + DIRTY_OFFSET;
        for (ULONG page_num = 0; page_num < page_count; page_num++) {
            if (*cur & DIRTY_BIT) {
                // found page with set soft-dirty bit
                if (SUCCESS != process_dirty_page(start + PAGE_SIZE * page_num, step_id)) {
                    free(entry);
                    return FAILURE;
                }
                dirty_found = 1;
            }
            cur += PAGE_DESCR_SIZE;
        }
    }

    free(entry);
    if (dirty_found) {
        return FOUND;
    }
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   mem_reset_dirty
 *
 *  Params:     pid
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Reset soft-dirty bit for all memory pages, by writing to
 *              /proc/pid/clear_refs.
 *              See "man 5 proc" and Linux kernel docs at
 *              Documentation/admin-guide/mm/soft-dirty.rst
 *
 **************************************************************************/
int mem_reset_dirty(void) {

    // TODO Can I keep the file opened to avoid opening/closing every time?
    FILE *clear_refs = fopen(clear_refs_filename, "w");
    if (!clear_refs) {
        ERR("Cannot open file '%s': %s", tmp, strerror(errno));
        return FAILURE;
    }
    char buf = '4';     // '4' resets soft-dirty bits
    if (!fwrite(&buf, 1, 1, clear_refs)) {
        ERR("Cannot write to file '%s'", tmp);
        fclose(clear_refs);
        return FAILURE;
    }

    fclose(clear_refs);
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   mem_current
 *
 *  Params:     address - starting address to read from
 *              size - how much to read
 *
 *  Return:     pointer to static buffer / NULL on error
 *
 *  Descr:      Read tracee memory
 *
 **************************************************************************/
const char *mem_current(uint64_t address, uint64_t size) {
    static size_t var_buf_size = 0;
    static char *var_buffer = NULL;

    struct iovec local, remote;
    if (size > var_buf_size) {      // auto grow variable value buffer to fit the biggest var
        free(var_buffer);
        /* it is important to use aligned momory as CPU instructions used by memdiff operates with aligned memory */
        if (posix_memalign((void *)&var_buffer, MEM_SEGMENT_SIZE, size)) {
            ERR("Out of memory");
            return NULL;
        }
        var_buf_size = size;
    }
    local.iov_base = var_buffer;
    local.iov_len = size;
    remote.iov_base = (void *)address;
    remote.iov_len = size;

    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) < (ssize_t)size) {
        ERR("Cannot read tracee memory: %s", strerror(errno));
        return NULL;
    }

    return var_buffer;
}


/**************************************************************************
 *
 *  Function:   mem_last_known
 *
 *  Params:     address - starting address to read from
 *
 *  Return:     pointer to static buffer / NULL on error
 *
 *  Descr:      Parent's copy of child's memory
 *
 **************************************************************************/
char *mapped_mem(uint64_t address) {
    for (struct region *cur = regions; cur; cur = cur->next) {
        if (cur->start <= address && cur->end > address) {
            div_t res = div(address - cur->start, PAGE_SIZE);
            return cur->pages[res.quot] + res.rem;
        }
    }

    return NULL;
}


#define MIN(A,B)    ((A) < (B) ? (A) : (B))
#define MAX(A,B)    ((A) > (B) ? (A) : (B))
/**************************************************************************
 *
 *  Function:   process_mem_regions
 *
 *  Params:     pid
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Read process memory map and create/update internal cache
 *
 **************************************************************************/
int mem_read_regions(void) {
    static char exe_name[PATH_MAX];

    char tmp[256];
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
    int refresh = 0;
    if (regions) {
        refresh = 1;
    }
    struct region **last = &regions;
    struct region *cur = regions;

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

            ULONG head, tail;
            /* first field, already 0-terminated, has start and end address */
            if (2 != sscanf(tmp, "%" PRIx64 "-%" PRIx64, &head, &tail)) {
                WARN("Cannot read memory regions");
                continue;
            }
            if (refresh && cur) {
                if (cur->start == head && cur->end == tail) {
                    last = &cur->next;
                    cur = cur->next;
                    continue;       // region didn't change
                }

                if (cur->end > head && cur->start < tail) {
                    /* read and cached regions differ, but intersect - update cached region */
                    int page_count = (tail - head) / PAGE_SIZE;
                    char **new_pages = malloc(page_count * sizeof(*new_pages));
                    int page_num = 0;

                    if (head < cur->start) {
                        DBG("Head grew");
                        /* region head grew - add new pages in front of cached ones */
                        page_count = (cur->start - head) / PAGE_SIZE;
                        char *pages;
                        /* it is important to use aligned momory as CPU instructions used by memdiff operates with aligned memory */
                        if (posix_memalign((void **)&pages, MEM_SEGMENT_SIZE, cur->start - head)) {
                            ERR("Out of memory");
                            free(new_pages);
                            fclose(maps);
                            return FAILURE;
                        }
                        memset(pages, 0, cur->start - head);
                        for (page_num = 0; page_num < page_count; page_num++) {
                            new_pages[page_num] = pages + (page_num * PAGE_SIZE);
                        }
                    }

                    /* process intersection */
                    page_count = (MIN(cur->end, tail) - MAX(cur->start, head)) / PAGE_SIZE;
                    int page_offset = 0;
                    if (head > cur->start) {
                        page_offset = (head - cur->start) / PAGE_SIZE;
                    }
                    for (int i = 0; i < page_count; i++) {
                        new_pages[page_num++] = cur->pages[i + page_offset];
                    }
                    /* NB! Some pages may become unreferenced here, and because they were allocated as one chunk, cannot free them */

                    if (tail > cur->end) {
                        DBG("Tail grew");
                        /* region tail grew - add new pages behind cached ones */
                        page_count = (tail - cur->end) / PAGE_SIZE;
                        char *pages;
                        /* it is important to use aligned momory as CPU instructions used by memdiff operates with aligned memory */
                        if (posix_memalign((void **)&pages, MEM_SEGMENT_SIZE, tail - cur->end)) {
                            ERR("Out of memory");
                            fclose(maps);
                            free(new_pages);
                            return FAILURE;
                        }
                        memset(pages, 0, cur->start - head);
                        for (int i = 0; i < page_count; i++) {
                            new_pages[page_num++] = pages + (i * PAGE_SIZE);
                        }
                        // coverity[leaked_storage] - in fact 'pages' doesn't leak
                    }
                    free(cur->pages);
                    cur->pages = new_pages;
                    last = &cur->next;
                    cur = cur->next;
                    continue;
                }

                /* got here only if region doesn't intersect with cached one - process it as new region */
                /* cur doesn't need to be changed in this case */
                DBG("New region");
            } else {
                DBG("New region");
            }

            /* either first read of completely new uncached region */
            struct region *new_reg = malloc(sizeof(*new_reg));
            if (!new_reg) {
                ERR("Out of memory");
                fclose(maps);
                return FAILURE;
            }
            new_reg->start = head;
            new_reg->end = tail;
            int page_count = (tail - head) / PAGE_SIZE;
            new_reg->pages = malloc(page_count * sizeof(*new_reg->pages));
            if (!new_reg->pages) {
                ERR("Out of memory");
                fclose(maps);
                free(new_reg);
                return FAILURE;
            }
            /* pages are not to be freed so allocate one big chunk for all the pages */
            char *pages;
            /* it is important to use aligned memory as CPU instructions used by memdiff operates with aligned memory */
            if (posix_memalign((void **)&pages, MEM_SEGMENT_SIZE, new_reg->end - new_reg->start)) {
                ERR("Out of memory");
                fclose(maps);
                free(new_reg->pages);
                free(new_reg);
                return FAILURE;
            }
            // coverity[tainted_data] - we can trust the data we read from /proc/<pid>/mem
            memset(pages, 0, new_reg->end - new_reg->start);
            for (int i = 0; i < page_count; i++) {
                new_reg->pages[i] = pages + (i * PAGE_SIZE);
            }
            if (refresh) {
                new_reg->next = cur;    // insert new region in front of current in cache
            } else {
                new_reg->next = NULL;
            }
            *last = new_reg;
            last = &new_reg->next;
        }
    }
    fclose(maps);

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   mem_in_cache
 *
 *  Params:     address - start address
 *              size
 *
 *  Return:     1 (hit) / 0 (miss)
 *
 *  Descr:      Check if specified chunk of memory is cached
 *
 **************************************************************************/
int mem_in_cache(uint64_t address, uint64_t size) {
    uint64_t end = address + size;
    for (struct region *cur = regions; cur; cur = cur->next) {
        if (address >= cur->start && end <= cur->end) {
            return 1;   // hit
        }
    }

    return 0;   // miss
}


/**************************************************************************
 *
 *  Function:   process_dirty_page
 *
 *  Params:     start - page start address
 *              step_id - current step
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find differences in page and store them in DB
 *
 **************************************************************************/
int process_dirty_page(ULONG start, ULONG step_id) {
    /* look through page segments, look for changed one */
    uint64_t page_end = start + PAGE_SIZE;
    for (uint64_t addr = start; addr < page_end; addr += MEM_SEGMENT_SIZE) {
        const char *mem = mem_current(addr, MEM_SEGMENT_SIZE);
        if (!mem) {
            return FAILURE;
        }
        char *local_addr = mapped_mem(addr);
        if (!*local_addr || memdiff(local_addr, mem, MEM_SEGMENT_SIZE)) {
            /* found changed segment, store it */
            memcpy(local_addr, mem, MEM_SEGMENT_SIZE);              // update local cache

            /* Store memory change using worker */
            struct insert_mem_msg *msg = malloc(sizeof(*msg));
            msg->address = addr;
            msg->step_id = step_id;
            memcpy(msg->content, mem, MEM_SEGMENT_SIZE);
            ch_write(insert_mem_ch, (char *)msg, sizeof(*msg));    // channel reader will free msg
        }
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_base_address
 *
 *  Params:     pid - process PID
 *              base - where to store base address
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find base address from program executable memory region
 *              address
 *
 **************************************************************************/
int get_base_address(pid_t p, uint64_t *base) {
    char exe_name[PATH_MAX];
    char tmp[256];

    // read executable name from /proc/<pid>/exe
    snprintf(tmp, sizeof(tmp), "/proc/%d/exe", p);
    ssize_t res = readlink(tmp, exe_name, sizeof(exe_name) - 1);
    if (res < 0) {
        ERR("Cannot get executable name from '%s': %s", tmp, strerror(errno));
        return FAILURE;
    }
    exe_name[res] = '\0';

    snprintf(tmp, sizeof(tmp), "/proc/%d/maps", p);
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
        if (!field || 'x' != field[2]) {     // skip memory without exec permissions
            continue;
        }
        int i = 2;  // two fields processed already
        while (i < 6 && NULL != (field = strtok_r(NULL, " \t\n", &state))) {
            i++;
        }
        if (field && !strcmp(field, exe_name)) {
            /* first field, already 0-terminated, has start and end address */
            sscanf(tmp, "%" PRIx64, base);
            break;
       }
    }
    fclose(maps);

    return SUCCESS;
}

