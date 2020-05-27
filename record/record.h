/**************************************************************************
 *
 *  File:       record.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Common stuff for 'record'
 *
 *  Notes:
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
#ifndef _RECORD_H
#define _RECORD_H

#include <stdio.h>
#include <sys/types.h>

#include "stingray.h"

#ifdef TIMING
#include <time.h>
struct timespec timer_started;
#define TIMER_START clock_gettime(CLOCK_MONOTONIC_RAW, &timer_started)
#define TIMER_STOP(msg) do { \
    struct timespec timer_stopped; \
    clock_gettime(CLOCK_MONOTONIC_RAW, &timer_stopped); \
    double diff = timer_stopped.tv_sec - timer_started.tv_sec + \
                            (timer_stopped.tv_nsec - timer_started.tv_nsec) / 1000000000.0; \
    INFO("%s took %.3lf sec", msg, diff); \
} while (0)
#else
#define TIMER_START
#define TIMER_STOP(msg)
#endif

/* linked list, used for storing names of units to include/exclude */
struct entry {
    char *name;
    struct entry *next;
};

int dbg_srcinfo(char *name);
int record(char *params[]);

int create_db(void);
int alter_db(void);
int prepare_statements(void);
sr_string get_abs_path(char *curdir, char *path);

extern void *insert_scope;
extern void *insert_line;
extern void *insert_func;
extern void *insert_type;
extern void *insert_member;
extern void *insert_var;
extern void *insert_var_decl;
extern void *update_var_loc;
extern void *insert_array;
extern void *select_type;

extern FILE            *logfd;
extern char            *acceptable_path;
extern struct entry    *ignore_unit;
extern struct entry    *process_unit;
extern int              unit_count;
extern uid_t            real_uid;
extern gid_t            real_gid;

#endif
