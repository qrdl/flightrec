/**************************************************************************
 *
 *  File:       db_workers.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Worker threads for writing/updating DB
 *
 *  Notes:      SQLite WAL mode results in locking when trying to run several
 *              transactions from different threads in parallel, even on
 *              different tables, therefore I have to use temporary
 *              DB. However when support for WAL2 journalling mode and BEGIN
 *              CONCURRENT is finally merged into SQLite trunk it may give
 *              better result then using temporary DB
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
#include "stingray.h"
#include "dab.h"

#include "flightrec.h"
#include "channel.h"
#include "db_workers.h"
#include "eel.h"

/* According to my measurments 4096 gives better performance than 2048 and 8192 */
#define COMMIT_FREQ     4096

extern char *db_name;


/**************************************************************************
 *
 *  Function:   wrk_insert_step
 *
 *  Params:     arg - channel to read messages from
 *
 *  Return:     (void *)1 / NULL on error
 *
 *  Descr:      Worker for inserting steps into DB
 *
 **************************************************************************/
void *wrk_insert_step(void *arg) {
    struct channel *ch = (struct channel *)arg;
    void *insert;
    size_t counter = 0;

    if (DAB_OK != DAB_OPEN("steps.fr", DAB_FLAG_CREATE)) {
        return NULL;
    }
    if (DAB_UNEXPECTED != DAB_EXEC("PRAGMA journal_mode=OFF")) {    // PRAGMA returns data we are not interested in
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("PRAGMA synchronous=OFF")) {    // PRAGMA returns data we are not interested in
        return NULL;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE step ("
                                "id             INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "address        INTEGER NOT NULL, "
                                "depth          INTEGER, "
                                "function_id    INTEGER, "     // ref function.id
                                "regs           BLOB"
                            ")")) {
        return NULL;
    }

    /* file_id and line are filled later, from address */
    if (DAB_OK != DAB_CURSOR_PREPARE(&insert, "INSERT "
                    "INTO step "
                    "(id, address, depth, function_id, regs) VALUES "
                    "(?,  ?,       ?,     ?,           ?)")) {
        return NULL;
    }

    struct insert_step_msg *msg;
    size_t size = sizeof(*msg);     // specify expected payload size
    if (DAB_OK != DAB_BEGIN) {
        return NULL;
    }
    struct sr registers;
    while (CHANNEL_OK == ch_read(ch, (char **)&msg, &size, READ_BLOCK)) {
        DAB_CURSOR_RESET(insert);
        /* manualy assemble Stingray string to be used as BLOB */
        registers.val = (char *)&msg->regs;
        registers.size = registers.len = sizeof(msg->regs);
        if (DAB_OK != DAB_CURSOR_BIND(insert,
                msg->step_id,
                msg->address,
                msg->depth,
                msg->func_id,
                &registers)) {
            DAB_ROLLBACK;
            return NULL;
        }
        if (DAB_NO_DATA != DAB_CURSOR_FETCH(insert)) {
            DAB_ROLLBACK;
            return NULL;
        }
        free(msg);
        counter++;
        if (counter >= COMMIT_FREQ) {
            if (DAB_OK != DAB_COMMIT) {
                DAB_ROLLBACK;
                return NULL;
            }
            if (DAB_OK != DAB_BEGIN) {
                return NULL;
            }
            counter = 0;
        }
    }
    if (DAB_OK != DAB_COMMIT) {
        DAB_ROLLBACK;
        return NULL;
    }

    DAB_CURSOR_FREE(insert);

    char tmp[256];
    sprintf(tmp, "ATTACH '%s' AS fr", db_name);
    if (DAB_OK != DAB_EXEC(tmp)) {
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("CREATE TABLE fr.step AS SELECT * FROM main.step")) {
        return NULL;
    }

    DAB_CLOSE(DAB_FLAG_NONE);

    return (void*)1;    // non-NULL means success
}


/**************************************************************************
 *
 *  Function:   wrk_insert_heap
 *
 *  Params:     arg - channel to read messages from
 *
 *  Return:     (void *)1 / NULL on error
 *
 *  Descr:      Worker for inserting/updating dynamic memory ops into DB
 *
 **************************************************************************/
void *wrk_insert_heap(void *arg) {
    struct channel *ch = (struct channel *)arg;
    void *insert, *update;
    size_t counter = 0;

    if (DAB_OK != DAB_OPEN("heap.fr", DAB_FLAG_CREATE)) {
        return NULL;
    }
    if (DAB_UNEXPECTED != DAB_EXEC("PRAGMA journal_mode=OFF")) {    // PRAGMA returns data we are not interested in
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("PRAGMA synchronous=OFF")) {    // PRAGMA returns data we are not interested in
        return NULL;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE heap ("
                                "address        INTEGER NOT NULL, "
                                "size           INTEGER NOT NULL, "
                                "allocated_at   INTEGER NOT NULL, "                 // ref step.id
                                "freed_at       INTEGER NOT NULL DEFAULT 0 "        // ref step.id
                            ")")) {
        return NULL;
    }
    if (DAB_OK != DAB_CURSOR_PREPARE(&insert, "INSERT "
            "INTO heap "
            "(address, size, allocated_at) VALUES "
            "(?,       ?,    ?)")) {
        return NULL;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&update, "UPDATE "
            "heap "
            "SET "
                "freed_at = ? "
            "WHERE "
                "address = ? AND "
                "freed_at = 0")) {
        return NULL;
    }

    struct insert_heap_msg *msg;
    size_t size = sizeof(*msg);     // specify expected payload size
    if (DAB_OK != DAB_BEGIN) {
        return NULL;
    }
    while (CHANNEL_OK == ch_read(ch, (char **)&msg, &size, READ_BLOCK)) {
        if (msg->size) {
            DAB_CURSOR_RESET(insert);
            if (DAB_OK != DAB_CURSOR_BIND(insert,
                    msg->address,
                    msg->size,
                    msg->step_id)) {
                DAB_ROLLBACK;
                return NULL;
            }
            if (DAB_NO_DATA != DAB_CURSOR_FETCH(insert)) {
                DAB_ROLLBACK;
                return NULL;
            }
        } else {
            DAB_CURSOR_RESET(update);
            if (DAB_OK != DAB_CURSOR_BIND(update,
                    msg->step_id,
                    msg->address)) {
                DAB_ROLLBACK;
                return NULL;
            }
            if (DAB_NO_DATA != DAB_CURSOR_FETCH(update)) {
                DAB_ROLLBACK;
                return NULL;
            }
        }

        free(msg);
        counter++;
        if (counter >= COMMIT_FREQ) {
            if (DAB_OK != DAB_COMMIT) {
                DAB_ROLLBACK;
                return NULL;
            }
            if (DAB_OK != DAB_BEGIN) {
                return NULL;
            }
            counter = 0;
        }
    }
    if (DAB_OK != DAB_COMMIT) {
        DAB_ROLLBACK;
        return NULL;
    }

    DAB_CURSOR_FREE(insert);
    DAB_CURSOR_FREE(update);

    char tmp[256];
    sprintf(tmp, "ATTACH '%s' AS fr", db_name);
    if (DAB_OK != DAB_EXEC(tmp)) {
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("CREATE TABLE fr.heap AS SELECT * FROM main.heap")) {
        return NULL;
    }

    DAB_CLOSE(DAB_FLAG_NONE);

    return (void*)1;    // non-NULL means success
}


/**************************************************************************
 *
 *  Function:   wrk_insert_mem
 *
 *  Params:     arg - channel to read messages from
 *
 *  Return:     (void *)1 / NULL on error
 *
 *  Descr:      Worker for inserting memory updates into DB
 *
 **************************************************************************/
void *wrk_insert_mem(void *arg) {
    struct channel *ch = (struct channel *)arg;
    void *insert;
    size_t counter = 0;

    if (DAB_OK != DAB_OPEN("mem.fr", DAB_FLAG_CREATE)) {     // already in multi-threaded mode
        return NULL;
    }
    if (DAB_UNEXPECTED != DAB_EXEC("PRAGMA journal_mode=OFF")) {    // PRAGMA returns data we are not interested in
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("PRAGMA synchronous=OFF")) {    // PRAGMA returns data we are not interested in
        return NULL;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE mem ("
                                "address        INTEGER NOT NULL, "
                                "step_id        INTEGER NOT NULL, "    // ref step.id
                                "content        BLOB "
                            ")")) {
        return NULL;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert, "INSERT "
            "INTO mem "
            "(address, step_id, content) VALUES "
            "(?,       ?,       ?)")) {
        return NULL ;
    }

    struct insert_mem_msg *msg;
    size_t size = sizeof(*msg);     // specify expected payload size
    if (DAB_OK != DAB_BEGIN) {
        return NULL;
    }
    struct sr content;
    while (CHANNEL_OK == ch_read(ch, (char **)&msg, &size, READ_BLOCK)) {
        DAB_CURSOR_RESET(insert);
        /* manualy assemble Stingray string to be used as BLOB */
        content.val = msg->content;
        content.size = content.len = sizeof(msg->content);
        if (DAB_OK != DAB_CURSOR_BIND(insert,
                msg->address,
                msg->step_id,
                &content)) {
            DAB_ROLLBACK;
            return NULL;
        }
        if (DAB_NO_DATA != DAB_CURSOR_FETCH(insert)) {
            DAB_ROLLBACK;
            return NULL;
        }
        free(msg);
        counter++;
        if (counter >= COMMIT_FREQ) {
            if (DAB_OK != DAB_COMMIT) {
                DAB_ROLLBACK;
                return NULL;
            }
            if (DAB_OK != DAB_BEGIN) {
                return NULL;
            }
            counter = 0;
        }
    }
    if (DAB_OK != DAB_COMMIT) {
        DAB_ROLLBACK;
        return NULL;
    }

    DAB_CURSOR_FREE(insert);

    char tmp[256];
    sprintf(tmp, "ATTACH '%s' AS fr", db_name);
    if (DAB_OK != DAB_EXEC(tmp)) {
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("CREATE TABLE fr.mem AS SELECT * FROM main.mem")) {
        return NULL;
    }
    if (DAB_OK != DAB_EXEC("CREATE UNIQUE INDEX fr.mem_by_address_and_step ON mem ("
                                "address, step_id)")) {
        return NULL;
    }

    DAB_CLOSE(DAB_FLAG_NONE);

    return (void*)1;    // non-NULL means success
}

