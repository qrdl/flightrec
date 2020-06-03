/**************************************************************************
 *
 *  File:       tester.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Functions used by tester (called from Bison-generated
 *              parser)
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
#ifndef _TESTER_H
#define _TESTER_H

#include <jsonapi.h>

#define SUCCESS 0
#define FAILURE 1

#define DATATYPE_STRING 1
#define DATATYPE_INT    2
#define DATATYPE_FLOAT  3
#define DATATYPE_BOOL   4

#define OP_JSON     1
#define OP_CONST    2

struct value {
    char datatype;  // one of DATATYPE_XXX contants
    char op_type;   // one of OP_XXX contants
    union {
        void *json;
        char *literal;
    };
};

int start(char *cmd_line, char **error);
int stop(char **error);

int request(const char *message, char **error);
int response(char **error);

void set_var(const char *name, const char *value);
const char *get_var(const char *name);

int match(const char *string, const char *pattern);

extern JSON_OBJ *json;

#endif
