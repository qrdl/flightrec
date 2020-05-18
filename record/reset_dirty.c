/**************************************************************************
 *
 *  File:       reset_dirty.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Control of special thread which resets memory page status
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
#include <pthread.h>
#include <errno.h>
#include <string.h>

#include "flightrec.h"
#include "eel.h"
#include "reset_dirty.h"

static void *reset_dirty(void *);

static FILE *clear_refs;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int busy;


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
    pthread_t reset_dirty_thread;
    if (pthread_create(&reset_dirty_thread, NULL, reset_dirty, NULL)) {
        ERR("Cannot start reset_dirty thread: %s", strerror(errno));
        return FAILURE;
    }
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
    pthread_cond_signal(&cond);
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
    if (busy) {
        /* wait for thread to release the mutex */
        int ret = pthread_mutex_lock(&mutex);
        if (ret) {
            ERR("Error locking mutex: %s", strerror(errno));
            return;
        }
        /* unlock mutex to allow it to be re-aquired by thread - thread will wait for condition */
        ret = pthread_mutex_unlock(&mutex);
        if (ret) {
            ERR("Error locking mutex: %s", strerror(errno));
            return;
        }
    }
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
void *reset_dirty(void* unused) {
    (void)unused;
    char buf = '4';     // '4' resets soft-dirty bits

    for (;;) {
        /* need to acquire the lock because pthread_cond_wait() requires mutex to be locked */
        int ret = pthread_mutex_lock(&mutex);
        if (ret) {
            ERR("Error locking mutex: %s", strerror(errno));
            return NULL;
        }

        /* wait to be triggered by trigger_reset_dirty() */
        ret = pthread_cond_wait(&cond, &mutex);
        if (ret) {
            ERR("Error waiting for condition: %s", strerror(errno));
            return NULL;
        }

        busy = 1;
        /* actual job */
        rewind(clear_refs);
        if (!fwrite(&buf, 1, 1, clear_refs)) {
            ERR("Cannot write to clear_refs file");
            return NULL;
        }
        busy = 0;

        /* indicate to main thread that run is completed */
        ret = pthread_mutex_unlock(&mutex);
        if (ret) {
            ERR("Error locking mutex: %s", strerror(errno));
            return NULL;
        }
    }

    return NULL;
}

