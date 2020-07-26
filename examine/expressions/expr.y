/**************************************************************************
 *
 *  File:       expr.y
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Grammar for expression parser
 *
 *  Notes:      Based on C standard grammar
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
%{
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "flightrec.h"
#include "expression.h"
#include "expr_internal.h"

#define IS_NUMERIC(A)   ((A) == TKIND_SIGNED || (A) == TKIND_UNSIGNED || (A) == TKIND_FLOAT)
#define IS_INTEGER(A)   ((A) == TKIND_SIGNED || (A) == TKIND_UNSIGNED)

#define CAST_TO_DOUBLE(A) ({ \
        struct ast_node *ast = new_ast_node(NODE_TYPE_TYPE); \
        ast->type_kind = TKIND_FLOAT; \
        ast->size = sizeof(double); \
        ast->operand = (A); \
        ast; })

#define CAST_TO_SIGNED(A) ({ \
        struct ast_node *ast = new_ast_node(NODE_TYPE_TYPE); \
        ast->type_kind = TKIND_SIGNED; \
        ast->size = sizeof(int64_t); \
        ast->operand = (A); \
        ast; })


int yylex(void);
/* yyerror() is standard function that may be called from inside YACC-egenrated code, while
   parse_error() is called from rules. Both functions set error_msg */
void yyerror(struct ast_node **node, const char *s);
void parse_error(const char *fmt, ...);

static void promote_operands_to_double(struct ast_node *ast, struct ast_node *left, struct ast_node *right);
static void promote_operands_to_signed(struct ast_node *ast, struct ast_node *left, struct ast_node *right);
static struct ast_node *binary_op(struct ast_node *left, struct ast_node *right, int op_type);

static char *error_msg = NULL;
static uint64_t scope;

%}

%union {
    struct ast_node *ast;
}

%token <ast> IDENTIFIER CONSTANT STRING_LITERAL TYPE VOID
%token PTR_OP LEFT_OP RIGHT_OP LE_OP GE_OP EQ_OP NE_OPAND_OP OR_OP AND_OP NE_OP SIZEOF

%type <ast> type expression logical_or_expression logical_and_expression inclusive_or_expression
%type <ast> exclusive_or_expression and_expression equality_expression relational_expression
%type <ast> shift_expression additive_expression multiplicative_expression cast_expression
%type <ast> unary_expression postfix_expression primary_expression

%start expression

%parse-param {struct ast_node **top}

%%

primary_expression
    : IDENTIFIER {
        if (SUCCESS != get_var_details(
                $1->var_name,
                scope,
                &$1->var_id,
                &$1->type_offset,
                &$1->type_kind,
                &$1->size,
                &$1->indirect)) {
            parse_error("Unknown variable '%s'", $1->var_name);
        }
        $$ = $1;
    }
    | CONSTANT
    | STRING_LITERAL
    | '(' expression ')' {
            $$ = $2;
        }
    ;

postfix_expression
    : primary_expression
    | postfix_expression '[' expression ']' {
            $$ = new_ast_node(NODE_TYPE_ITEM);
            $$->object = $1;
            $$->member = $3;
            /* make sure object is subscriptable, and index is countable */
            if (!$1->indirect && $1->type_kind != TKIND_ARRAY) {
                parse_error("Object is non-subscriptable");
            } else if ($3->type_kind != TKIND_SIGNED && $3->type_kind != TKIND_UNSIGNED) {
                parse_error("Array/pointer subscript isn't countable");
            } else {
                /* figure out base type */
                if (TKIND_ARRAY == $1->type_kind) {
                    if (SUCCESS != get_base_type_details(
                            $1->type_offset,
                            &$$->type_offset,
                            &$$->type_kind,
                            &$$->size,
                            &$$->indirect)) {
                        parse_error("Cannot find array/pointer base type");
                    }
                } else {    // pointer
                    $$->type_offset = $1->type_offset;
                    $$->type_kind = $1->type_kind;
                    $$->size = $1->size;
                    $$->indirect = $1->indirect - 1;    // dereference
                }
            }
        }
    | postfix_expression '.' IDENTIFIER {
            $$ = new_ast_node(NODE_TYPE_FIELD);
            $$->object = $1;
            $$->member = $3;
            /* find member */
            if (SUCCESS != get_field_details(
                    $3->var_name,
                    $1->type_offset,
                    &$$->type_offset,
                    &$$->type_kind,
                    &$$->size,
                    &$$->start,
                    &$$->indirect)) {
                parse_error("Cannot find field '%s' in struct/union", $3->var_name);
            }
        }
    | postfix_expression PTR_OP IDENTIFIER {
            /* for '->' operator create extra node for dereference */
            $$ = new_ast_node(NODE_TYPE_FIELD);
            $$->object = new_ast_node(NODE_TYPE_UNARY_OP);
            $$->object->op_code = OP_DEREF;
            $$->object->left = $1;
            $$->member = $3;
            if (!$1->indirect) {
                parse_error("Cannot dereference non-pointer");
            } else if ($1->indirect > 1) {
                parse_error("Not enough dereferences");
            } else {
                $$->object->type_offset = $1->type_offset;
                $$->object->type_kind = $1->type_kind;
                $$->object->size = $1->size;
                $$->object->indirect = 0;
                if (SUCCESS != get_field_details(
                        $3->var_name,
                        $1->type_offset,
                        &$$->type_offset,
                        &$$->type_kind,
                        &$$->size,
                        &$$->start,
                        &$$->indirect)) {
                    parse_error("Cannot find field '%s' in struct/union", $3->var_name);
                }
            }
        }
    ;

unary_expression
    : postfix_expression
    | '+' cast_expression {
            $$ = $2;
        }
    | '&' cast_expression {
            $$ = new_ast_node(NODE_TYPE_UNARY_OP);
            $$->op_code = OP_ADDR;
            $$->left = $2;
            if ($2->node_type != NODE_TYPE_VAR && $2->node_type != NODE_TYPE_FIELD && $2->node_type != NODE_TYPE_ITEM) {
                parse_error("Cannot take an address on non-lvalue");
            } else {
                $$->type_offset = $2->type_offset;
                $$->type_kind = $2->type_kind;
                $$->size = $2->size;
                $$->indirect = $2->indirect + 1;
            }
        }
    | '*' cast_expression {
            $$ = new_ast_node(NODE_TYPE_UNARY_OP);
            $$->op_code = OP_DEREF;
            $$->left = $2;
            if (!$2->indirect) {
                parse_error("Cannot dereference non-pointer");
            } else {
                $$->type_offset = $2->type_offset;
                $$->type_kind = $2->type_kind;
                $$->size = $2->size;
                $$->indirect = $2->indirect - 1;
            }
        }
    | '-' cast_expression {
            $$ = new_ast_node(NODE_TYPE_UNARY_OP);
            $$->op_code = OP_NEG;
            $$->left = $2;
            if (    ($2->type_kind != TKIND_SIGNED && $2->type_kind != TKIND_UNSIGNED && $2->type_kind != TKIND_FLOAT) ||
                    $2->indirect) {
                parse_error("Cannot apply unary minus to non-number");
            } else if (TKIND_FLOAT == $2->type_kind) {
                $$->type_kind = TKIND_FLOAT;
                $$->size = sizeof(double);
            } else {
                $$->type_kind = TKIND_SIGNED;
                $$->size = sizeof(int64_t);
            }
        }
    | '~' cast_expression {
            $$ = new_ast_node(NODE_TYPE_UNARY_OP);
            $$->op_code = OP_INV;
            $$->left = $2;
            if (($2->type_kind != TKIND_SIGNED && $2->type_kind != TKIND_UNSIGNED) || $2->indirect) {
                parse_error("Cannot invert non-number");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(int64_t);
            }
        }
    | '!' cast_expression {
            $$ = new_ast_node(NODE_TYPE_UNARY_OP);
            $$->op_code = OP_NOT;
            $$->left = $2;
            if ($2->type_kind != TKIND_SIGNED && $2->type_kind != TKIND_UNSIGNED && $2->indirect == 0) {
                parse_error("Cannot negate non-number/non-pointer");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(int64_t);
            }
        }
    | SIZEOF unary_expression {
            $$ = new_ast_node(NODE_TYPE_INT);
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(size_t);
            $$->int_value = $2->size;
            free_ast_node($2);
        }
    | SIZEOF '(' type ')' {
            $$ = new_ast_node(NODE_TYPE_INT);
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(size_t);
            $$->int_value = $3->size;
            free_ast_node($3);
        }
    ;

cast_expression
    : unary_expression
    | '(' type ')' cast_expression {
            /* compare source type and target type to check validity of the case */
            if (    ($2->indirect && $4->indirect) ||                                               // pointer to pointer
                    ($2->indirect && $4->type_kind == TKIND_ARRAY) ||                               // array to pointer
                    ($2->indirect && IS_NUMERIC($4->type_kind)) ||                                  // integer to pointer
                    ($4->indirect && IS_INTEGER($2->type_kind) && $2->size == sizeof(uint64_t)) ||  // pointer to long int
                    (IS_NUMERIC($2->type_kind) && IS_NUMERIC($4->type_kind))) {                     // number to number, possibly loosing sign and/or precision
                $$ = $2;
                $$->operand = $4;
            } else {
                parse_error("Unsupported types for cast");
            }
        }
    ;


multiplicative_expression
    : cast_expression
    | multiplicative_expression '*' cast_expression {
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform multiplication on non-numbers");
            } else {
                $$ = binary_op($1, $3, OP_MUL);
            }
        }
    | multiplicative_expression '/' cast_expression {
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform division on non-numbers");
            } else {
                $$ = binary_op($1, $3, OP_DIV);
            }
        }
    | multiplicative_expression '%' cast_expression {
            if (!IS_INTEGER($1->type_kind) || !IS_INTEGER($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform modulo on non-integers");
            }
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_MOD;
            $$->type_kind = TKIND_SIGNED;
            $$->size = sizeof(int64_t);
            if (TKIND_UNSIGNED == $1->type_kind) {
                $$->left = CAST_TO_SIGNED($1);
            } else {
                $$->left = $1;
            }
            if (TKIND_UNSIGNED == $3->type_kind) {
                $$->right = CAST_TO_SIGNED($3);
            } else {
                $$->right = $3;
            }
        }
    ;

additive_expression
    : multiplicative_expression
    | additive_expression '+' multiplicative_expression {
            if ($1->indirect) {
                if ($3->indirect || !IS_INTEGER($3->type_kind)) {
                    parse_error("Second operand for pointer addition must be integer");
                } else {                
                    $$ = new_ast_node(NODE_TYPE_BINARY_OP);
                    $$->indirect = $1->indirect;
                    $$->type_kind = $1->type_kind;
                    $$->type_offset = $1->type_offset;
                    $$->size = $1->size;
                    $$->op_code = OP_ADD;
                    $$->left = $1;
                    if (TKIND_SIGNED != $3->type_kind) {
                        $$->right = CAST_TO_SIGNED($3);
                    } else {
                        $$->right = $3;
                    }
                }
            } else if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $3->indirect) {
                parse_error("Cannot perform addition on non-numbers");
            } else {
                $$ = binary_op($1, $3, OP_ADD);
            }
        }
    | additive_expression '-' multiplicative_expression {
            if ($1->indirect) {
                if ($3->indirect) {
                    if ($1->indirect != $3->indirect || $1->size != $3->size) {
                        parse_error("Pointer subtraction operands must be of the matching types");
                    } else {
                        $$ = new_ast_node(NODE_TYPE_BINARY_OP);
                        $$->type_kind = TKIND_SIGNED;
                        $$->size = sizeof(int64_t);
                        $$->op_code = OP_SUB;
                        $$->left = $1;
                        $$->right = $3;
                    }
                } else if (IS_INTEGER($3->type_kind)) {
                    $$ = new_ast_node(NODE_TYPE_BINARY_OP);
                    $$->indirect = $1->indirect;
                    $$->type_kind = $1->type_kind;
                    $$->type_offset = $1->type_offset;
                    $$->size = $1->size;
                    $$->op_code = OP_SUB;
                    $$->left = $1;
                    if (TKIND_SIGNED != $3->type_kind) {
                        $$->right = CAST_TO_SIGNED($3);
                    } else {
                        $$->right = $3;
                    }
                } else {
                    parse_error("Second operand for pointer subtraction must be pointer or integer");
                }
            } else if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $3->indirect) {
                parse_error("Cannot perform subtraction on non-numbers");
            } else {
                $$ = binary_op($1, $3, OP_SUB);
            }
        }
    ;

shift_expression
    : additive_expression
    | shift_expression LEFT_OP additive_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_LEFT;
            /* shift if defined only for positive numbers.
               Operands still can be of signed type, but it will be cast to unsigned */
            if (!IS_INTEGER($1->type_kind) || !IS_INTEGER($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform shift on non-integers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(uint64_t);
            }
        }
    | shift_expression RIGHT_OP additive_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_RIGHT;
            /* shift if defined only for positive numbers.
               Operands still can be of signed type, but it will be cast to unsigned */
            if (!IS_INTEGER($1->type_kind) || !IS_INTEGER($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform shift on non-integers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(uint64_t);
            }
        }
    ;

relational_expression
    : shift_expression
    | relational_expression '<' shift_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_LT;
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(int64_t);
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform comparison of non-numbers");
            } else if (TKIND_FLOAT == $1->type_kind || TKIND_FLOAT == $3->type_kind) {
                promote_operands_to_double($$, $1, $3);
            } else if (TKIND_SIGNED == $1->type_kind || TKIND_SIGNED == $3->type_kind) {
                promote_operands_to_signed($$, $1, $3);
            } else {
                $$->left = $1;
                $$->right = $3;
            }
        }
    | relational_expression '>' shift_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_GT;
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(int64_t);
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform comparison of non-numbers");
            } else if (TKIND_FLOAT == $1->type_kind || TKIND_FLOAT == $3->type_kind) {
                promote_operands_to_double($$, $1, $3);
            } else if (TKIND_SIGNED == $1->type_kind || TKIND_SIGNED == $3->type_kind) {
                promote_operands_to_signed($$, $1, $3);
            } else {
                $$->left = $1;
                $$->right = $3;
            }
        }
    | relational_expression LE_OP shift_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_LE;
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(int64_t);
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform comparison of non-numbers");
            } else if (TKIND_FLOAT == $1->type_kind || TKIND_FLOAT == $3->type_kind) {
                promote_operands_to_double($$, $1, $3);
            } else if (TKIND_SIGNED == $1->type_kind || TKIND_SIGNED == $3->type_kind) {
                promote_operands_to_signed($$, $1, $3);
            } else {
                $$->left = $1;
                $$->right = $3;
            }
        }
    | relational_expression GE_OP shift_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_GE;
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(int64_t);
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform comparison of non-numbers");
            } else if (TKIND_FLOAT == $1->type_kind || TKIND_FLOAT == $3->type_kind) {
                promote_operands_to_double($$, $1, $3);
            } else if (TKIND_SIGNED == $1->type_kind || TKIND_SIGNED == $3->type_kind) {
                promote_operands_to_signed($$, $1, $3);
            } else {
                $$->left = $1;
                $$->right = $3;
            }
        }
    ;

equality_expression
    : relational_expression
    | equality_expression EQ_OP relational_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_EQ;
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(int64_t);
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform comparison of non-numbers");
            } else if (TKIND_FLOAT == $1->type_kind || TKIND_FLOAT == $3->type_kind) {
                promote_operands_to_double($$, $1, $3);
            } else if (TKIND_SIGNED == $1->type_kind || TKIND_SIGNED == $3->type_kind) {
                promote_operands_to_signed($$, $1, $3);
            } else {
                $$->left = $1;
                $$->right = $3;
            }
        }
    | equality_expression NE_OP relational_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->op_code = OP_NEQ;
            $$->type_kind = TKIND_UNSIGNED;
            $$->size = sizeof(int64_t);
            if (!IS_NUMERIC($1->type_kind) || !IS_NUMERIC($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform comparison of non-numbers");
            } else if (TKIND_FLOAT == $1->type_kind || TKIND_FLOAT == $3->type_kind) {
                promote_operands_to_double($$, $1, $3);
            } else if (TKIND_SIGNED == $1->type_kind || TKIND_SIGNED == $3->type_kind) {
                promote_operands_to_signed($$, $1, $3);
            } else {
                $$->left = $1;
                $$->right = $3;
            }
        }
    ;

and_expression
    : equality_expression
    | and_expression '&' equality_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_BIT_AND;
            if (!IS_INTEGER($1->type_kind) || !IS_INTEGER($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform bitwise logical op of non-numbers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(uint64_t);
            }
        }
    ;

exclusive_or_expression
    : and_expression
    | exclusive_or_expression '^' and_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_XOR;
            if (!IS_INTEGER($1->type_kind) || !IS_INTEGER($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform bitwise logical op of non-numbers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(uint64_t);
            }
        }
    ;

inclusive_or_expression
    : exclusive_or_expression
    | inclusive_or_expression '|' exclusive_or_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_BIT_OR;
            if (!IS_INTEGER($1->type_kind) || !IS_INTEGER($3->type_kind) || $1->indirect || $3->indirect) {
                parse_error("Cannot perform bitwise logical op of non-numbers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(uint64_t);
            }
        }
    ;

logical_and_expression
    : inclusive_or_expression
    | logical_and_expression AND_OP inclusive_or_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_AND;
            /* for logical op operands can be of different types, so leave delaing with types to evaluator */
            if ((!IS_NUMERIC($1->type_kind) && !$1->indirect) || (!IS_NUMERIC($3->type_kind) && !$3->indirect)) {
                parse_error("Cannot perform logical operation of non-numbers/non-pointers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(int64_t);
            }
        }
    ;

logical_or_expression
    : logical_and_expression
    | logical_or_expression OR_OP logical_and_expression {
            $$ = new_ast_node(NODE_TYPE_BINARY_OP);
            $$->left = $1;
            $$->right = $3;
            $$->op_code = OP_OR;
            /* for logical op operands can be of different types, so leave delaing with types to evaluator */
            if ((!IS_NUMERIC($1->type_kind) && !$1->indirect) || (!IS_NUMERIC($3->type_kind) && !$3->indirect)) {
                parse_error("Cannot perform logical operation of non-numbers/non-pointers");
            } else {
                $$->type_kind = TKIND_UNSIGNED;
                $$->size = sizeof(int64_t);
            }
        }
    ;

expression
    : logical_or_expression { *top = $1; }
    ;

type
    : VOID
    | TYPE
    | type '*' {
            $$->indirect++;
        }
    ;

%%

void promote_operands_to_double(struct ast_node *ast, struct ast_node *left, struct ast_node *right) {
    if (TKIND_FLOAT != left->type_kind) {
        ast->left = CAST_TO_DOUBLE(left);
        ast->right = right;
    } else if (TKIND_FLOAT != right->type_kind) {
        ast->left = left;
        ast->right = CAST_TO_DOUBLE(right);
    } else {
        ast->left = left;
        ast->right = right;
    }
}

void promote_operands_to_signed(struct ast_node *ast, struct ast_node *left, struct ast_node *right) {
    if (TKIND_SIGNED != left->type_kind) {
        ast->left = CAST_TO_SIGNED(left);
        ast->right = right;
    } else if (TKIND_SIGNED != right->type_kind) {
        ast->left = left;
        ast->right = CAST_TO_SIGNED(right);
    } else {
        ast->left = left;
        ast->right = right;
    }
}

struct ast_node *binary_op(struct ast_node *left, struct ast_node *right, int op_type) {
    struct ast_node *res;

    res = new_ast_node(NODE_TYPE_BINARY_OP);
    res->op_code = op_type;
    if (TKIND_FLOAT == left->type_kind || TKIND_FLOAT == right->type_kind) {
        res->type_kind = TKIND_FLOAT;
        res->size = sizeof(double);
        promote_operands_to_double(res, left, right);
    } else if (TKIND_SIGNED == left->type_kind || TKIND_SIGNED == right->type_kind) {
        res->type_kind = TKIND_SIGNED;
        res->size = sizeof(int64_t);
        promote_operands_to_signed(res, left, right);
    } else {
        /* subtraction may result in negative value even for unsigned values */
        if (OP_SUB == op_type) {
            res->type_kind = TKIND_SIGNED;
            res->size = sizeof(uint64_t);
        } else {
            res->type_kind = TKIND_UNSIGNED;
            res->size = sizeof(uint64_t);
        }
        res->left = left;
        res->right = right;
    }

    return res;
}


extern void yy_scan_string(const char *);

void parse_error(const char *fmt, ...) {
    if (error_msg) {
        // some error already reported
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap) + 1;
    va_end(ap);
    error_msg = malloc(len);
    va_start(ap, fmt);
    vsnprintf(error_msg, len, fmt, ap);
    va_end(ap);
}

void yyerror(struct ast_node **node, char const *s) {
    (void)(node);
    if (!error_msg) {
        error_msg = strdup(s);
    }
}

struct ast_node *expr_parse(const char *expr, uint64_t scope_id, char **msg) {
#if YYDEBUG
    yydebug = 1;
#endif
    if (error_msg) {
        free(error_msg);
        error_msg = NULL;
    }
    yy_scan_string(expr);
    struct ast_node *ast = NULL;
    scope = scope_id;   // needed to resolve variables
    yyparse(&ast);
    if (error_msg) {
        free_ast_node(ast);
        *msg = error_msg;
        return NULL;
    }
    *msg = NULL;

    return ast;
}
