/**************************************************************************
 *
 *  File:       mem_workers.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Worker for resetting mem page soft-dirty flag
 *
 *  Notes:      Counter-intuitively having one thread waiting to be
 *              triggered is significantly cheaper then creating new thread
 *              every time
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
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "flightrec.h"
#include "eel.h"
#include "reset_dirty.h"

static void *reset_dirty(void *);

static FILE *clear_refs;
static sem_t start_sem, end_sem;

/**************************************************************************
 *
 *  Function:   start_reset_dirty
 *
 *  Params:     pid
 *
 *  Return:     FAILURE / SUCCESS
 *
 *  Descr:      Start the worker thread
 *
 **************************************************************************/
int start_reset_dirty(pid_t pid) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/proc/%d/clear_refs", pid);
    clear_refs = fopen(tmp, "w");
    if (!clear_refs) {
        ERR("Cannot open file '%s': %s", tmp, strerror(errno));
        return FAILURE;
    }
    if (sem_init(&start_sem, 0, 0)) {
        ERR("Cannot init the semaphoe for BPF thread sync: %s", strerror(errno));
        return FAILURE;
    }
    if (sem_init(&end_sem, 0, 0)) {
        ERR("Cannot init the semaphoe for BPF thread sync: %s", strerror(errno));
        return FAILURE;
    }
    pthread_t reset_dirty_thread;
    if (pthread_create(&reset_dirty_thread, NULL, reset_dirty, NULL)) {
        ERR("Error starting reset_dirty thread: %s", strerror(errno));
        return FAILURE;
    }
    pthread_setname_np(reset_dirty_thread, "fr_dirty");
    INFO("Reset dirty worker thread started");
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   trigger_reset_dirty
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Trigger worker thread run
 *
 **************************************************************************/
void trigger_reset_dirty(void) {
    if (sem_post(&start_sem)) {
        ERR("Cannot increment semaphore: %s", strerror(errno));
    }
}


/**************************************************************************
 *
 *  Function:   wait_reset_dirty
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Wait for worker thread to complete the run
 *
 **************************************************************************/
void wait_reset_dirty(void) {
    if (sem_wait(&end_sem)) {
        ERR("Cannot decrement semaphore: %s", strerror(errno));
    }
}


/**************************************************************************
 *
 *  Function:   reset_dirty
 *
 *  Params:     unused
 *
 *  Return:     NULL
 *
 *  Descr:      Thread function - reset soft-dirty flag for all memory
 *              pages to force page faults on any change
 *
 **************************************************************************/
void *reset_dirty(void* unused) {
    (void)unused;
    char buf = '4';     // '4' resets soft-dirty bits

    for (;;) {
        /* wait for trigger */
        if (sem_wait(&start_sem)) {
            ERR("Cannot decrement semaphore: %s", strerror(errno));
            return NULL;
        }

        /* actual job */
        rewind(clear_refs);
        if (!fwrite(&buf, 1, 1, clear_refs)) {
            ERR("Cannot write to clear_refs file");
            return NULL;
        }

        /* signal end of processing */
        if (sem_post(&end_sem)) {
            ERR("Cannot increment semaphore: %s", strerror(errno));
            return NULL;
        }
    }

    return NULL;
}
