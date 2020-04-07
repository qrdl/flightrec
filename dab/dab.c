/**************************************************************************
 *
 *  File:       dab.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Database access library
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
#include <sqlite3.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include "eel.h"
#include "stingray.h"
#include "dab.h"

#define STMT(A) ((sqlite3_stmt *)(A))
#define P_STMT(A) ((sqlite3_stmt **)(A))
#define UNUSED(A) (void)(A)

#define DBERR(A) do { \
        sqlite3 *db = DB_HANDLE; \
        LOCAL_LOG('E', file, line, "%s - %s (%d)", (A), sqlite3_errmsg(db), sqlite3_errcode(db)); \
} while (0)

static int sql_common(const char *file, int line, sqlite3_stmt **stmt, const char *stmt_text, va_list ap);
static int cursor_bind(const char *file, int line, sqlite3_stmt *stmt, va_list ap);

static int threads = 0;
static pthread_key_t thread_key;
static sqlite3 *global_db = NULL;      // DB handle used for single-threaded use
#define DB_HANDLE (threads ? (sqlite3 *)pthread_getspecific(thread_key) : global_db)

/**************************************************************************
 *
 *  Function:   dab_open
 *
 *  Params:     db_name - database file name
 *              user (not used)
 *              password - not used
 *              flags - see DAB_FLAG_XXX
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL / DAB_MALFUNCTION
 *
 *  Descr:      Connect to db using provided credentials
 *
 **************************************************************************/
int dab_open(const char *file, int line, const char *db_name, ULONG flags) {
    int ret;

    if (!db_name) {
        return DAB_INVALID;
    }

    int sqlite_flags = 0;
    if (flags & DAB_FLAG_READONLY) {
        sqlite_flags |= SQLITE_OPEN_READONLY;
    } else {
        sqlite_flags |= SQLITE_OPEN_READWRITE;
    }
    if (flags & DAB_FLAG_CREATE) {
        sqlite_flags |= SQLITE_OPEN_CREATE;
    }

    if (flags & DAB_FLAG_THREADS && !threads) {     // run only once
        if (!sqlite3_threadsafe()) {
            LOCAL_LOG('E', file, line, "SQLite is compiled without thread support");
            return DAB_FAIL;
        }
        ret = pthread_key_create(&thread_key, NULL);
        if (ret) {
            LOCAL_LOG('E', file, line, "Cannot create thread-specific variable: %s", strerror(ret));
            return DAB_MALFUNCTION;
        }
        ret = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
        if (SQLITE_OK != ret) {
            LOCAL_LOG('E', file, line, "Cannot switch on multi-threading: %s (%d)", sqlite3_errstr(ret), ret);
            return DAB_FAIL;
        }
        threads = 1;
    }

    /* create/open ODB */
    sqlite3 *db;
    ret = sqlite3_open_v2(db_name, &db, sqlite_flags, NULL);
    if (SQLITE_OK != ret) {
        LOCAL_LOG('E', file, line, "DB open failed: %s (%d)", sqlite3_errstr(ret), ret);
        if (SQLITE_CANTOPEN == ret) {
            return DAB_INVALID;
        } else {
            return DAB_FAIL;
        }
    }

    if (threads) {
        ret = pthread_setspecific(thread_key, db);
        if (ret) {
            LOCAL_LOG('E', file, line, "Cannot set thread-specific variable: %s", strerror(ret));
            return DAB_MALFUNCTION;
        }
    } else {
        global_db = db;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_close
 *
 *  Params:     flags - see DAB_FLAG_DEFER
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_MALFUNCTION
 *
 *  Descr:      Close DB connection
 *
 *  Note:       In case of graceful close connection still be active
 *              until all prepared statements are freed, but will not be
 *              reachable for new operations, DB needs to be re-opened
 *              to run new statements.
 *
 **************************************************************************/
int dab_close(const char *file, int line, int flag) {
    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return DAB_OK;      // already closed - nothing to do
    }

    if (DAB_FLAG_GRACEFUL & flag) {
        if (SQLITE_OK != sqlite3_close_v2(db)) {
            DBERR("Error closing DB");
            return DAB_FAIL;
        }
    } else {    // close immediate
        /* rollback active transaction */
        if (!sqlite3_get_autocommit(db)) {   // indicates whether in txn or not
            LOCAL_LOG('W', file, line, "Active transaction is rolled back because of DB close");
            dab_rollback(file, line);
        }

        /* destory all prepared statements */
        sqlite3_stmt *stmt;
        for (stmt = sqlite3_next_stmt(db, NULL); stmt; stmt = sqlite3_next_stmt(db, NULL)) {
#ifdef DEBUG
            LOCAL_LOG('D', file, line, "Finalising prepared statement because of DB close: %s", sqlite3_sql(stmt));
#endif
            sqlite3_finalize(stmt);
        }

        /* actual close */
        if (SQLITE_OK != sqlite3_close(db)) {
            DBERR("Error closing DB");
            return DAB_FAIL;
        }
    }

    if (threads) {
        int thread_err = pthread_setspecific(thread_key, NULL);
        if (thread_err) {
            LOCAL_LOG('E', file, line, "Cannot set thread-specific variable: %s", strerror(thread_err));
            return DAB_MALFUNCTION;
        }
    } else if (db == global_db) {
        global_db = NULL;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_exec
 *
 *  Params:     stmt_text - SQL statement text
 *              ... - pair of params, each pair consists of type ID and
 *                    parameter
 *                    type 0 means "no more params"
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL
 *
 *  Descr:      My own SQLite one-function exec that does everything -
 *              prepares the statement, binds all params, executes the
 *              statement and frees it
 *              Because statement is prepared, there is no need to protect
 *              from SQL injection
 *
 **************************************************************************/
int dab_exec(const char *file, int line, const char *stmt_text, ...) {
    int ret;
    sqlite3_stmt *stmt = NULL;
    va_list ap;

    if (!stmt_text) {
        return DAB_INVALID;
    }

    va_start(ap, stmt_text);

    ret = sql_common(file, line, &stmt, stmt_text, ap);
    if (DAB_OK != ret) {
        RETCLEAN(ret);
    }

    ret = sqlite3_step(stmt);
    if (SQLITE_DONE != ret) {
        if (SQLITE_ROW == ret) {
            RETCLEAN(DAB_UNEXPECTED);
        }
        DBERR("Error executing statement");
        RETCLEAN(DAB_FAIL);
    }
    ret = DAB_OK;

cleanup:
    va_end(ap);

    if (stmt) {
        sqlite3_finalize(stmt);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   dab_cursor_open
 *
 *  Params:     stmt (OUT) - where to store prepared statement
 *              stmt_text - SQL statement text
 *              ... - pair of params, each pair consists of type ID and
 *                    parameter
 *                    type 0 means "no more params"
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL
 *
 *  Descr:      Create and bind the cursor
 *
 **************************************************************************/
int dab_cursor_open(const char *file, int line, void **stmt, const char *stmt_text, ...) {
    int ret;
    va_list ap;

    if (!stmt || !stmt_text) {
        return DAB_INVALID;
    }

    va_start(ap, stmt_text);

    ret = sql_common(file, line, P_STMT(stmt), stmt_text, ap);

    va_end(ap);

    return ret;
}


/**************************************************************************
 *
 *  Function:   dab_cursor_prepare
 *
 *  Params:     stmt (OUT) - where to store prepared statement
 *              stmt - SQL statement text
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_INVALID
 *
 *  Descr:      Common code for dab_exec() and dab_cursor_open()
 *
 **************************************************************************/
int dab_cursor_prepare(const char *file, int line, void **stmt, const char *stmt_text) {
    if (!stmt || !stmt_text) {
        return DAB_INVALID;
    }

    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return DAB_INVALID;
    }

    if (SQLITE_OK != sqlite3_prepare_v2(db, stmt_text, -1, P_STMT(stmt), NULL)) {
        DBERR("Error preparing statement");
        return DAB_FAIL;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_last_id
 *
 *  Params:
 *
 *  Return:     rowid of last inserted record / 0 on error
 *
 *  Descr:      Return ID of last inserted record, if it has AUTOINCREMENT
 *              column
 *
 **************************************************************************/
ULONG dab_last_id(const char *file, int line) {
    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return 0;
    }

    ULONG row = sqlite3_last_insert_rowid(db);
    if (!row) {
        DBERR("Nothing was inserted into DB");
        return 0;
    }

    return row;
}


/**************************************************************************
 *
 *  Function:   dab_affected_rows
 *
 *  Params:
 *
 *  Return:     number of rows
 *
 *  Descr:      Return numer of rows affected by last successful
 *              INSERT / UPDATE / DELETE
 *
 **************************************************************************/
ULONG dab_affected_rows(void) {
    sqlite3 *db = DB_HANDLE;
    return sqlite3_changes(db);
}


/**************************************************************************
 *
 *  Function:   dab_cursor_bind
 *
 *  Params:     stmt - prepared SQL statement
 *              ... - pair of params, each pair consists of type ID and
 *                    parameter
 *                    type 0 means "no more params"
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL
 *
 *  Descr:      Re-bind previously created cursor
 *
 **************************************************************************/
int dab_cursor_bind(const char *file, int line, void *stmt, ...) {
    va_list ap;
    int ret;

    if (!stmt)
        return DAB_INVALID;

    va_start(ap, stmt);

    ret = cursor_bind(file, line, STMT(stmt), ap);

    va_end(ap);

    return ret;
}

#define TMP(A) A *tmp = va_arg(ap, A *)
/**************************************************************************
 *
 *  Function:   dab_cursor_fetch
 *
 *  Params:     stmt - prepared and bound SQL statement
 *              ... - pair of params, each pair consists of type ID and
 *                    parameter
 *                    type 0 means "no more params"
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL / DAB_NO_DATA
 *
 *  Descr:      Fetch a row from cursor
 *
 **************************************************************************/
int dab_cursor_fetch(const char *file, int line, void *stmt, ...) {
    int ret;
    va_list ap;
    int type, index = 0;

    if (!stmt) {
        return DAB_INVALID;
    }

    va_start(ap, stmt);

    ret = sqlite3_step(STMT(stmt));
    if (SQLITE_DONE == ret) {
        RETCLEAN(DAB_NO_DATA);
    } else if (SQLITE_ROW != ret) {
        DBERR("Error fetching stmt");
        RETCLEAN(DAB_FAIL);
    }
    ret = DAB_OK;

    for (type = va_arg(ap, int); type; type = va_arg(ap, int)) {
        switch (type) {
            case GEN_DATATYPE_INT: {
                    TMP(int);
                    *tmp = sqlite3_column_int(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_UINT: {
                    TMP(unsigned int);
                    *tmp = sqlite3_column_int(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_SHRT: {
                    TMP(short);
                    *tmp = sqlite3_column_int(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_USHRT: {
                    TMP(unsigned short);
                    *tmp = sqlite3_column_int(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_LNG: {
                    TMP(long);
                    if (8 == sizeof(long)) {
                        *tmp = sqlite3_column_int64(STMT(stmt), index);
                    } else {
                        *tmp = sqlite3_column_int(STMT(stmt), index);
                    }
                    break;
                }
            case GEN_DATATYPE_ULNG: {
                    TMP(unsigned long);
                    if (8 == sizeof(unsigned long)) {
                        *tmp = sqlite3_column_int64(STMT(stmt), index);
                    } else {
                        *tmp = sqlite3_column_int(STMT(stmt), index);
                    }
                    break;
                }
            case GEN_DATATYPE_LLNG: {
                    TMP(long long);
                    if (8 == sizeof(long long)) {
                        *tmp = sqlite3_column_int64(STMT(stmt), index);
                    } else {
                        *tmp = sqlite3_column_int(STMT(stmt), index);
                    }
                    break;
                }
            case GEN_DATATYPE_ULLNG: {
                    TMP(unsigned long long);
                    if (8 == sizeof(unsigned long long)) {
                        *tmp = sqlite3_column_int64(STMT(stmt), index);
                    } else {
                        *tmp = sqlite3_column_int(STMT(stmt), index);
                    }
                    break;
                }
            case GEN_DATATYPE_CHR: {
                    TMP(char);
                    *tmp = sqlite3_column_int(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_UCHR: {
                    TMP(unsigned char);
                    *tmp = sqlite3_column_int(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_STR:
            case GEN_DATATYPE_USTR: {
                    TMP(char *);
                    const char * zzz = (const char *)sqlite3_column_text(STMT(stmt), index);
                    if (zzz) {
                        *tmp = strdup(zzz);
                    } else {
                        *tmp = NULL;
                    }
                    break;
                }
            case GEN_DATATYPE_FLT: {
                    TMP(float);
                    *tmp = sqlite3_column_double(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_DBL: {
                    TMP(double);
                    *tmp = sqlite3_column_double(STMT(stmt), index);
                    break;
                }
            case GEN_DATATYPE_SR: {
                    sr_string tmp = va_arg(ap, sr_string);
                    size_t size = sqlite3_column_bytes(STMT(stmt), index);
                    sr_ensure_size(tmp, size, size);
                    memcpy(CSTR(tmp), sqlite3_column_blob(STMT(stmt), index), size);
                    SETLEN(tmp, size);
                    break;
                }
            default:
                DBERR("Invalid column type");
                RETCLEAN(DAB_INVALID);
                break;
        }
        index++;
    }

cleanup:
    va_end(ap);

    return ret;
}


/**************************************************************************
 *
 *  Function:   dab_cursor_reset
 *
 *  Params:     stmt - prepared and bound SQL statement
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL
 *
 *  Descr:      Reset the cursor, but do not destroy it. To be used again,
 *              cursor must be bound first
 *
 **************************************************************************/
int dab_cursor_reset(const char *file, int line, void *stmt) {
    if (!stmt) {
        return DAB_INVALID;
    }

    if (SQLITE_OK != sqlite3_reset(STMT(stmt)) || SQLITE_OK != sqlite3_clear_bindings(STMT(stmt))) {
        DBERR("Error resetting stmt");
        return DAB_FAIL;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_cursor_free
 *
 *  Params:     stmt - prepared SQL statement
 *
 *  Return:     DAB_OK / DAB_INVALID / DAB_FAIL
 *
 *  Descr:      Destory the cursor
 *
 **************************************************************************/
int dab_cursor_free(const char *file, int line, void *stmt) {
    if (!stmt) {
        return DAB_INVALID;
    }

    if (SQLITE_OK != sqlite3_finalize(STMT(stmt))) {
        DBERR("Error freeing stmt");
        return DAB_FAIL;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   sql_common
 *
 *  Params:     stmt (OUT) - where to store prepared statement
 *              stmt - SQL statement text
 *              ap - va_list of params
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_INVALID / DAB_MALFUNCTION
 *
 *  Descr:      Common code for dab_exec() and dab_cursor_open()
 *
 **************************************************************************/
int sql_common(const char *file, int line, sqlite3_stmt **stmt, const char *stmt_text, va_list ap) {
    int ret;

    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return DAB_INVALID;
    }

    if (SQLITE_OK != sqlite3_prepare_v2(db, stmt_text, -1, stmt, NULL)) {
        DBERR("Error preparing statement");
        return DAB_FAIL;
    }

    ret = cursor_bind(file, line, *stmt, ap);

    return ret;
}


/**************************************************************************
 *
 *  Function:   cursor_bind
 *
 *  Params:     stmt - prepared SQL statement
 *              ap - va_list of params
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_INVALID
 *
 *  Descr:      Common code for dab_cursor_bind() and sql_common()
 *
 **************************************************************************/
int cursor_bind(const char *file, int line, sqlite3_stmt *stmt, va_list ap) {
    int type, index = 0;
    int ret;

    for (type = va_arg(ap, int); type; type = va_arg(ap, int)) {
        index++;
        switch (type) {
            case GEN_DATATYPE_INT:
            case GEN_DATATYPE_SHRT:
            case GEN_DATATYPE_CHR:
                ret = sqlite3_bind_int(stmt, index, va_arg(ap, int));
                break;
            case GEN_DATATYPE_UINT:
            case GEN_DATATYPE_USHRT:
            case GEN_DATATYPE_UCHR:
                ret = sqlite3_bind_int(stmt, index, va_arg(ap, unsigned int));
                break;
            case GEN_DATATYPE_LNG:
                if (8 == sizeof(long))
                    ret = sqlite3_bind_int64(stmt, index, va_arg(ap, long));
                else
                    ret = sqlite3_bind_int(stmt, index, va_arg(ap, long));
                break;
            case GEN_DATATYPE_ULNG:
                if (8 == sizeof(unsigned long)) {
                    ret = sqlite3_bind_int64(stmt, index, va_arg(ap, unsigned long));
                } else {
                    ret = sqlite3_bind_int(stmt, index, va_arg(ap, unsigned long));
                }
                break;
            case GEN_DATATYPE_LLNG:
                if (8 == sizeof(long long)) {
                    ret = sqlite3_bind_int64(stmt, index, va_arg(ap, long long));
                } else {
                    ret = sqlite3_bind_int(stmt, index, va_arg(ap, long));
                }
                break;
            case GEN_DATATYPE_ULLNG:
                if (8 == sizeof(unsigned long long)) {
                    ret = sqlite3_bind_int64(stmt, index, va_arg(ap, unsigned long long));
                } else {
                    ret = sqlite3_bind_int(stmt, index, va_arg(ap, unsigned long long));
                }
                break;
            case GEN_DATATYPE_STR:
            case GEN_DATATYPE_USTR:
                ret = sqlite3_bind_text(stmt, index, va_arg(ap, char*), -1, NULL);
                break;
            case GEN_DATATYPE_FLT:
            case GEN_DATATYPE_DBL:
                ret = sqlite3_bind_double(stmt, index, va_arg(ap, double));
                break;
            case GEN_DATATYPE_SR: {
                sr_string tmp = va_arg(ap, sr_string);
                ret = sqlite3_bind_text(stmt, index, CSTR(tmp), STRLEN(tmp),
                        NULL);
                break;
            }
            default:
                LOCAL_LOG('E', file, line, "Invalid column type for param %d", index);
                return DAB_INVALID;
                break;
        }
        if (SQLITE_OK != ret) {
            DBERR("Error binding param");
            return DAB_FAIL;
        }
    }

    /* check if number of '?' placeholders actually match the number of
     * params */
    if (sqlite3_bind_parameter_count(stmt) != index) {
        LOCAL_LOG('E', file, line, "Number of params doesn't match number of placeholders");
        return DAB_INVALID;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_begin
 *
 *  Params:     txn - pointer to local transaction indicator
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_INVALID / DAB_MALFUNCTION
 *
 *  Descr:      Begin local transaction
 *
 **************************************************************************/
int dab_begin(const char *file, int line) {
    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return DAB_INVALID;
    }

    if (sqlite3_get_autocommit(db)) {   // indicates whether in txn or not
        if (SQLITE_OK != sqlite3_exec(db, "BEGIN TRANSACTION", NULL, 0, NULL)) {
            DBERR("Cannot start transaction");
            return DAB_FAIL;
        }
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_commit
 *
 *  Params:     txn - pointer to local transaction indicator
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_INVALID / DAB_MALFUNCTION
 *
 *  Descr:      Commit local transaction
 *
 **************************************************************************/
int dab_commit(const char *file, int line) {
    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return DAB_INVALID;
    }

    if (SQLITE_OK != sqlite3_exec(db, "COMMIT", NULL, 0, NULL)) {
        DBERR("Cannot commit transaction");
        return DAB_FAIL;
    }

    return DAB_OK;
}


/**************************************************************************
 *
 *  Function:   dab_rollback
 *
 *  Params:     txn - pointer to local transaction indicator
 *
 *  Return:     DAB_OK / DAB_FAIL / DAB_INVALID / DAB_MALFUNCTION
 *
 *  Descr:      Rollback local transaction
 *
 **************************************************************************/
int dab_rollback(const char *file, int line) {
    sqlite3 *db = DB_HANDLE;
    if (!db) {
        return DAB_INVALID;
    }

    if (SQLITE_OK != sqlite3_exec(db, "ROLLBACK", NULL, 0, NULL)) {
        DBERR("Cannot rollback transaction");
        return DAB_FAIL;
    }

    return DAB_OK;
}

