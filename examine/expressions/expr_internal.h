/**************************************************************************
 *
 *  File:       expr_internal.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Expression parsing and evaluation - data structures,
 *              constants and internal functions
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
#ifndef _EXPR_INTERNAL_H
#define _EXPR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/**** unary operators ****/
#define OP_ADDR     1
#define OP_DEREF    2
#define OP_NEG      3
#define OP_INV      4
#define OP_NOT      5

/**** binary operators ****/
/* math */
#define OP_MATH_MIN 6
#define OP_MUL      6
#define OP_DIV      7
#define OP_MOD      8
#define OP_ADD      9
#define OP_SUB      10
#define OP_MATH_MAX 10

/* bitwise */
#define OP_BIT_MIN  11
#define OP_BIT_AND  11
#define OP_XOR      12
#define OP_BIT_OR   13
#define OP_LEFT     14
#define OP_RIGHT    15
#define OP_BIT_MAX  15

/* logic */
#define OP_AND      16
#define OP_OR       17

/* relational */
#define OP_REL_MIN  18
#define OP_LT       18
#define OP_GT       19
#define OP_LE       20
#define OP_GE       21
#define OP_EQ       22
#define OP_NEQ      23
#define OP_REL_MAX  23

/* sizeof - evaluated during parsing */
#define OP_SIZEOF   24

#define NODE_TYPE_INT       1
#define NODE_TYPE_FLOAT     2
#define NODE_TYPE_STRING    3
#define NODE_TYPE_VAR       4
#define NODE_TYPE_FIELD     5
#define NODE_TYPE_ITEM      6
#define NODE_TYPE_UNARY_OP  7
#define NODE_TYPE_BINARY_OP 8
#define NODE_TYPE_TYPE      9


struct ast_node {
    int         node_type;
    /* datatype attrbites */
    int64_t     type_offset;    // can be negative for casting to basic type
    int         type_kind;      // one of TKIND_XXX constants
    size_t      size;
    int         indirect;       // number of indirections, e.g. how many dereferences needed to get the value
    union {
        struct {            // number - NODE_TYPE_INT or NODE_TYPE_FLOAT
            union {
                int64_t int_value;
                double  float_value;
            };
        };
        char *str_value;    // NODE_TYPE_STRING
        struct {            // NODE_TYPE_VAR
            char        *var_name;
            uint64_t    var_id;
        };
        struct {            // NODE_TYPE_FIELD or NODE_TYPE_ITEM
            struct ast_node *object;
            struct ast_node *member;
            uint64_t        start;
        };
        struct {            // NODE_TYPE_UNARY_OP or NODE_TYPE_BINARY_OP
            int             op_code;
            struct ast_node *left;
            struct ast_node *right;
        };
        struct ast_node *operand;   // NODE_TYPE_TYPE
    };
};

int get_var_details(const char *name, uint64_t scope, uint64_t *var_id, int64_t *type_offset, int *kind, size_t *size,
                    int *indirect);
int get_base_type_details(int64_t offset, int64_t *type_offset, int *kind, size_t *size, int *indirect);
int get_field_details(const char *name, int64_t type, int64_t *type_offset, int *kind, size_t *size, uint64_t *start,
                    int *indirect);
int get_struct_details(const char *name, int64_t *offset, int *kind, size_t *size);
int get_type_details(const char *name, int64_t *offset, int *kind, size_t *size);

#endif
