/**************************************************************************
 *
 *  File:       examine.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      'Examine' tool common definitions
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
#ifndef _EXAMINE_H
#define _EXAMINE_H

#include "flightrec.h"
#include "jsonapi.h"

#define SUCCESS     0
#define FAILURE     1

/* parent types for references */
#define PTYPE_SCOPE 1
#define PTYPE_REF   2
#define PTYPE_EXPR  3

#define MEM_NOTFOUND    2
#define MEM_RELEASED    3

int init_comms(char *port);
int read_message(int fd, char **message);
int send_message(int fd, const char *message);

int open_dbginfo(const char *filename);
int add_var(ULONG scope, JSON_OBJ *container, ULONG var_id, ULONG step);
int add_var_items(JSON_OBJ *container, ULONG ref_id, unsigned int start, unsigned int count);
int add_var_fields(JSON_OBJ *container, ULONG ref_id);

int get_var_address(uint64_t var_id, uint64_t step, char **name, uint64_t *address, uint64_t *type_offset);
char *get_var_value(ULONG addr, size_t size, uint64_t step);
int get_var_ref(int parent_type, ULONG parent, const char *child, ULONG address, ULONG type, int indirect, ULONG *ref);
struct sr *type_name(ULONG type_offset, int indirect);
int get_pointer_size(ULONG address, ULONG *size);

void release_cursors(void);

extern int listener;

extern uint64_t cur_step;
extern uint64_t program_base_addr;

// these statements are used only in var_value.c, but need to be released in requests.c
extern void *var_cursor;
extern void *step_cursor;
extern void *array_cursor;
extern void *struct_cursor;
extern void *member_cursor;
extern void *mem_cursor;
extern void *type_cursor;
extern void *ref_cursor;
extern void *ref_insert;
extern void *ref_update;
extern void *heap_cursor;
extern void *func_cursor;
extern void *type_name_cursor;
extern void *enum_cursor;

#endif

