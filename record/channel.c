/**************************************************************************
 *
 *  File:       channel.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Inter-thread channels
 *
 *  Notes:      Reading from and writing to channel are synchronised so
 *              simultaneous reading and writing is possible (even from
 *              multiple readers and writers). Reading blocks until there
 *              something to read from channel
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
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

#include "eel.h"
#include "channel.h"

struct message {
    void            *payload;
    size_t          size;
    struct message  *next;
};

struct channel {
    sem_t           sem;
    pthread_mutex_t mutex;
    struct message  *head;
    struct message  *tail;
};


/**************************************************************************
 *
 *  Function:   ch_create
 *
 *  Params:     N/A
 *
 *  Return:     newely-created channel / NULL
 *
 *  Descr:      Create a new channel
 *
 **************************************************************************/
struct channel *ch_create(void) {
    struct channel *ch = malloc(sizeof(*ch));
    ch->head = ch->tail = NULL;
    if (0 != sem_init(&ch->sem, 0, 0)) {
        ERR("Cannot initialise the semaphore: %s", strerror(errno));
        return NULL;
    }
    if (0 != pthread_mutex_init(&ch->mutex, NULL)) {
        ERR("Cannot initialise the mutex: %s", strerror(errno));
        return NULL;
    }

    return ch;
}


/**************************************************************************
 *
 *  Function:   ch_write
 *
 *  Params:     ch - channel
 *              buf - message buffer
 *              bufsize - message size
 *
 *  Return:     CHANNEL_OK / CHANNEL_FAIL
 *
 *  Descr:      Write to channel
 * 
 *  Note:       Message is not copied, it means that writer cannot destroy
 *              the message after calling ch_write(). It is responsibility
 *              of the reader to destroy message after use!
 *
 **************************************************************************/
int ch_write(struct channel *ch, char *buf, size_t bufsize) {
    if (0 != pthread_mutex_lock(&ch->mutex)) {
        ERR("Cannot lock the mutex: %s", strerror(errno));
        return CHANNEL_FAIL;
    }
    struct message *msg = malloc(sizeof(*msg));
    msg->payload = buf;
    msg->size = bufsize;
    msg->next = NULL;
    if (ch->tail) {
        ch->tail->next = msg;
        ch->tail = msg;
    } else {
        ch->tail = ch->head = msg;
    }
    if (0 != pthread_mutex_unlock(&ch->mutex)) {
        ERR("Cannot unlock the mutex: %s", strerror(errno));
        return CHANNEL_FAIL;
    }

    if (0!= sem_post(&ch->sem)) {       // signal the reader
        ERR("Cannot increment the semaphore: %s", strerror(errno));
        return CHANNEL_FAIL;
    }

    return CHANNEL_OK;
}


/**************************************************************************
 *
 *  Function:   ch_read
 *
 *  Params:     ch - channel
 *              buf - where to store the message
 *              bufsize - where to store the message size
 *
 *  Return:     CHANNEL_OK / CHANNEL_FAIL / CHANNEL_END
 *
 *  Descr:      Read from channel
 * 
 *  Notes:      Reading blocks if there is nothing to read.
 *              Returns CHANNEL_END when get empty message.
 *              It is responsibility of the reader to destroy message after use
 *
 **************************************************************************/
int ch_read(struct channel *ch, char **buf, size_t *bufsize) {
    if (0 != sem_wait(&ch->sem)) {      // wait for data
        ERR("Cannot decrement the semaphore: %s", strerror(errno));
        return CHANNEL_FAIL;
    }

    if (0 != pthread_mutex_lock(&ch->mutex)) {
        ERR("Cannot lock the mutex: %s", strerror(errno));
        return CHANNEL_FAIL;
    }

    struct message *msg = ch->head;

    *buf = msg->payload;
    size_t size = msg->size;
    ch->head = msg->next;
    if (!ch->head) {
        ch->tail = NULL;
    }
    free(msg);

    if (0 != pthread_mutex_unlock(&ch->mutex)) {
        ERR("Cannot unlock the mutex: %s", strerror(errno));
        return CHANNEL_FAIL;
    } 

    if (!*buf) {
        *bufsize = 0;
        return CHANNEL_END;
    }

    // if expected size is specified, payload must match it
    if (*bufsize && *bufsize != size) {
        ERR("Message in channel is %zu bytes big (expected %zu) - skipping", size, *bufsize);
        free(*buf);
        *bufsize = 0;
        *buf = NULL;
        return CHANNEL_MISREAD;
    }

    *bufsize = size;

    return CHANNEL_OK;
}


/**************************************************************************
 *
 *  Function:   ch_finish
 *
 *  Params:     ch - channel
 *
 *  Return:     CHANNEL_OK / CHANNEL_FAIL
 *
 *  Descr:      signal the reader the end of communications
 *
 **************************************************************************/
int ch_finish(struct channel *ch) {
    return ch_write(ch, NULL, 0);
}


/**************************************************************************
 *
 *  Function:   ch_destroy
 *
 *  Params:     ch - channel
 *
 *  Return:     N/A
 *
 *  Descr:      Deallocate all resources, taken by the channel, including
 *              messages, if there are any
 *
 **************************************************************************/
void ch_destroy(struct channel *ch) {
    pthread_mutex_destroy(&ch->mutex);
    sem_destroy(&ch->sem);
    for (struct message *msg = ch->head; msg; msg = ch->head) {
        ch->head = msg->next;
        free(msg->payload);
        free(msg);
    }
    free(ch);
}

