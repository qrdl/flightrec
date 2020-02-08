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
#ifndef _RECORD_H
#define _RECORD_H

#include <stdio.h>

#include "stingray.h"

/* linked list, used for storing names of units to include/exclude */
struct entry {
    char *name;
    struct entry *next;
};

int dbg_srcinfo(char *name);
int record(char *fr_path, char *params[]);

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
extern void *select_line;

extern FILE            *logfd;
extern char            *acceptable_path;
extern struct entry    *ignore_unit;
extern struct entry    *process_unit;

#endif
