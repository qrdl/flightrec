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
#ifndef DAB_H
#define DAB_H

#include <generics.h>

#define DAB_OK              1
#define DAB_FAIL            2
#define DAB_NO_DATA         3
#define DAB_INVALID         4
#define DAB_MALFUNCTION     5
#define DAB_UNEXPECTED      6

#define DAB_FLAG_NONE       0
#define DAB_FLAG_READONLY   1
#define DAB_FLAG_CREATE     2
#define DAB_FLAG_GRACEFUL   4
#define DAB_FLAG_THREADS    8

/*
 * Database manipulation
 */
#define DAB_OPEN(D, F) dab_open(__FILE__, __LINE__, (D), (F))
#define DAB_CLOSE(F) dab_close(__FILE__, __LINE__, F)

/*
 * Execution of SQL statements
 */

/* One-step excution */
#define DAB_EXEC(A, ...) dab_exec(__FILE__, __LINE__, (A), VAR(NUMARGS(__VA_ARGS__), ##__VA_ARGS__))

/* Queries using cursor */
#define DAB_CURSOR_OPEN(C, A, ...) dab_cursor_open(__FILE__, __LINE__, (C), (A), VAR(NUMARGS(__VA_ARGS__), ##__VA_ARGS__))
#define DAB_CURSOR_PREPARE(C, A, ...) dab_cursor_prepare(__FILE__, __LINE__, (C), (A))
#define DAB_CURSOR_BIND(C, ...) dab_cursor_bind(__FILE__, __LINE__, (C), VAR(NUMARGS(__VA_ARGS__), ##__VA_ARGS__))
#define DAB_CURSOR_FETCH(C, ...) dab_cursor_fetch(__FILE__, __LINE__, (C), PVAR(NUMARGS(__VA_ARGS__), ##__VA_ARGS__))
#define DAB_CURSOR_RESET(C) dab_cursor_reset(__FILE__, __LINE__, (C))
#define DAB_CURSOR_FREE(C) do { dab_cursor_free(__FILE__, __LINE__, (C)); C = NULL;} while (0)

/* Last inserted autoincremented ID */
#define DAB_LAST_ID dab_last_id(__FILE__, __LINE__)

/* Number of affected rows */
#define DAB_AFFECTED_ROWS dab_affected_rows()

/*
 * Transactions
 */
#define DAB_BEGIN dab_begin(__FILE__, __LINE__)
#define DAB_COMMIT dab_commit(__FILE__, __LINE__)
#define DAB_ROLLBACK dab_rollback(__FILE__, __LINE__)

/* these functions must be called using DAB_XXX macros, not directly */
int dab_open(const char *file, int line, const char *db_name, ULONG flags);
int dab_close(const char *file, int line, int flag);
int dab_exec(const char *file, int line, const char *stmt_text, ...);
int dab_cursor_open(const char *file, int line, void **cursor, const char *stmt_text, ...);
int dab_cursor_prepare(const char *file, int line, void **cursor, const char *stmt_text);
int dab_cursor_bind(const char *file, int line, void *cursor, ...);
int dab_cursor_fetch(const char *file, int line, void *cursor, ...);
int dab_cursor_reset(const char *file, int line, void *cursor);
int dab_cursor_free(const char *file, int line, void *cursor);
ULONG dab_last_id(const char *file, int line);
ULONG dab_affected_rows(void);

int dab_begin(const char *file, int line);
int dab_commit(const char *file, int line);
int dab_rollback(const char *file, int line);

#endif

