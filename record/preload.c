/**************************************************************************
 *
 *  File:       preload.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Intercept dynamic memory manipulations in tracee
 *
 *  Notes:      All intercepted calls are sent to tracer via named pipe.
 *              Cannot use printf family of functions because it may call
 *              malloc internally therefore print all errors using write()
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
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
// for P_tmpdir
#ifndef __USE_XOPEN
#define __USE_XOPEN 1
#endif
#include <stdio.h>

#include "mem.h"

#define SEND_ERROR_MSG   "Cannot send event: 0x"
#define OPEN_ERROR_MSG   "Cannot open pipe: 0x"
#define CREATE_ERROR_MSG "Cannot create pipe: 0x"

int fifo_fd = 0;

/* these functions aren't publicly declared so manually declare it here */
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);
void* __libc_valloc(size_t);
void* __libc_pvalloc(size_t);
void  __libc_free(void*);
int __posix_memalign(void **, size_t, size_t);

/**************************************************************************
 *
 *  Function:   int_to_hex_string
 *
 *  Params:     source - integer value to convert
 *              target - pre-allocated string to write to
 *
 *  Return:     N/A
 *
 *  Descr:      Convert integer to hex string
 *
 **************************************************************************/
static void int_to_hex_string(int source, char *target) {
    unsigned char bytes[sizeof(source)];
    memcpy(bytes, &source, sizeof(source));
    // print errno code as hex value, inverting the byte order
    int significant = 0;
    int cur = 0;
    for (int i = sizeof(bytes) - 1; i >= 0; i--) {
        char high_nibble = (bytes[i] & 0xF0) >> 4;
        if (high_nibble >= 0 && high_nibble <= 9) {
            high_nibble += '0';
        } else {
            high_nibble += 'A' - 10;
        }
        char low_nibble = bytes[i] & 0x0F;
        if (low_nibble >= 0 && low_nibble <= 9) {
            low_nibble += '0';
        } else {
            low_nibble += 'A' - 10;
        }
        /* avoid printing leading insignificant zeros */
        if (significant || high_nibble != '0') {
            target[cur++] = high_nibble;
            significant = 1;
        }
        if (significant || low_nibble != '0') {
            target[cur++] = low_nibble;
            significant = 1;
        }
    }
    target[cur] = '\0';
}


/**************************************************************************
 *
 *  Function:   send_alloc_event
 *
 *  Params:     address - address of allocated memory chunk
 *              size - size of allocated memory chunk
 *
 *  Return:     N/A
 *
 *  Descr:      Send allocation event to tracer via named pipe
 *
 **************************************************************************/
static void send_alloc_event(uint64_t address, uint64_t size) {
    struct heap_event event;
    event.type = HEAP_EVENT_ALLOC;
    event.size = size;
    event.address = address;

    if (write(fifo_fd, &event, sizeof(event)) < 0) {
        write(2, SEND_ERROR_MSG, sizeof(SEND_ERROR_MSG)-1);
        char errcode[17];
        int_to_hex_string(errno, errcode);
        write(2, errcode, strlen(errcode));
        char eol = '\n';
        write(2, &eol, 1);
    }
}


/**************************************************************************
 *
 *  Function:   send_free_event
 *
 *  Params:     address - address of deallocated memory chunk
 *
 *  Return:     N/A
 *
 *  Descr:      Send deallocation event to tracer via named pipe
 *
 **************************************************************************/
static void send_free_event(uint64_t address) {
    struct heap_event event;
    event.type = HEAP_EVENT_FREE;
    event.size = 0;
    event.address = address;

    if (write(fifo_fd, &event, sizeof(event)) < 0) {
        write(2, SEND_ERROR_MSG, sizeof(SEND_ERROR_MSG)-1);
        char errcode[17];
        int_to_hex_string(errno, errcode);
        write(2, errcode, strlen(errcode));
        char eol = '\n';
        write(2, &eol, 1);
    }
}


/**************************************************************************
 *
 *  Function:   init
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Open named pipe to tracee at startup
 *
 **************************************************************************/
static __attribute__((constructor)) void init(void) {
    char fifo_name[256] = "";
    strcpy(fifo_name, P_tmpdir);
    strcat(fifo_name, "/fr_");
    char pidstr[17];
    int_to_hex_string(getpid(), pidstr);
    strcat(fifo_name, pidstr);

    /* By this time tracer should have already created the pipe, but to be on the
       safe side create it here as well */
    if (mkfifo(fifo_name, S_IRUSR | S_IWUSR) && EEXIST != errno) {
        write(2, CREATE_ERROR_MSG, sizeof(CREATE_ERROR_MSG)-1);
        char errcode[17];
        int_to_hex_string(errno, errcode);
        write(2, errcode, strlen(errcode));
        char eol = '\n';
        write(2, &eol, 1);
        return;
    }

    fifo_fd = open(fifo_name, O_WRONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        write(2, OPEN_ERROR_MSG, sizeof(OPEN_ERROR_MSG)-1);
        char errcode[17];
        int_to_hex_string(errno, errcode);
        write(2, errcode, strlen(errcode));
        char eol = '\n';
        write(2, &eol, 1);
    }
}


/**************************************************************************
 *
 * Wrappers around standard library functions that call standard functions
 * and inform tracer
 *
 **************************************************************************/
void *malloc(size_t size) {
    void *res = __libc_malloc(size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, size);
    }
    return res;
}

void *calloc(size_t nmemb, size_t size) {
    void *res = __libc_calloc(nmemb, size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, nmemb * size);
    }
    return res;
}

void *realloc(void *ptr, size_t size) {
    void *res = __libc_realloc(ptr, size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, size);
    }
    return res;
}

void *memalign(size_t alignment, size_t size) {
    void *res = __libc_memalign(alignment, size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, size);
    }
    return res;
}

void *aligned_alloc(size_t alignment, size_t size) {
    void *res = __libc_memalign(alignment, size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, size);
    }
    return res;
}

void *valloc(size_t size) {
    void *res = __libc_valloc(size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, size);
    }
    return res;
}

void *pvalloc(size_t size) {
    void *res = __libc_pvalloc(size);
    if (res && fifo_fd) {
        send_alloc_event((uint64_t)res, size);
    }
    return res;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    int res = __posix_memalign(memptr, alignment, size);
    if (!res && fifo_fd) {
        send_alloc_event((uint64_t)*memptr, size);
    }
    return res;

}

void free(void *ptr) {
    if (fifo_fd) {
        send_free_event((uint64_t)ptr);
    }
    __libc_free(ptr);
}

