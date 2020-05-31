/**************************************************************************
 *
 *  File:       expr.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Expression parsing and evaluation
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
#ifndef _EXPR_H
#define _EXPR_H

#include <stddef.h>
#include <stdint.h>

#include "jsonapi.h"

struct ast_node;    // real definition inside expr_internal.h

struct ast_node *new_ast_node(int type);
void free_ast_node(struct ast_node *node);

struct ast_node *expr_parse(const char *expr, uint64_t scope_id, char **error);
int get_eval_result(JSON_OBJ *container, uint64_t id, struct ast_node *ast, uint64_t step, char **error);

int query_expr_cache(const char *expr_text, uint64_t *id, struct ast_node **ast);
int update_expr_cache(uint64_t id, struct ast_node *ast);

void close_expr_cursors(void);

#endif
