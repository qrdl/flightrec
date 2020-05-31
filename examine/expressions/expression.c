/**************************************************************************
 *
 *  File:       expression.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Expression parsing and evaluations
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
#include <stdlib.h>
#include <stdint.h>

#include "flightrec.h"
#include "dab.h"
#include "eel.h"
#include "jsonapi.h"

#include "examine.h"
#include "expression.h"
#include "expr_internal.h"

#define NO_FLAGS    0
#define FLAG_ADDR   1

union node_value {
    int64_t     signed_value;
    uint64_t    unsigned_value;
    double      float_value;
    void        *pointer_value;
};

static union node_value evaluate_node(struct ast_node *ast, uint64_t step, int flags, char **error);
static union node_value float_bin_op(struct ast_node *ast, uint64_t step, char **error);
static union node_value signed_bin_op(struct ast_node *ast, uint64_t step, char **error);
static union node_value unsigned_bin_op(struct ast_node *ast, uint64_t step, char **error);
static union node_value var_value(struct ast_node *ast, uint64_t addr, uint64_t step, char **error);
static int type_details(uint64_t type_offset, uint64_t *dim, uint64_t *type_kind);

static void *expr_struct_cursor;
static void *expr_type_cursor;
static void *expr_var_cursor;
static void *expr_field_cursor;
static void *expr_basetype_cursor;
static void *expr_addexpr_cursor;
static void *expr_getexpr_cursor;
static void *expr_updexpr_cursor;
static void *expr_typedetails_cursor;


/**************************************************************************
 *
 *  Function:   get_eval_result
 *
 *  Params:     container - JSON object to add value to
 *              ast - tree/sub-tree to evaluate
 *              step - step to find variable values
 *              error - where to store error message
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Evaluate expression and add result to JSON
 *
 **************************************************************************/
int get_eval_result(JSON_OBJ *container, uint64_t id, struct ast_node *ast, uint64_t step, char **error) {
    int ret = SUCCESS;
    struct sr *tname;

    /* for basic type get variable value and format according to type */
    if (!ast->indirect && ast->type_kind >= TKIND_BASIC_MIN && ast->type_kind <= TKIND_BASIC_MAX) {
        union node_value res = evaluate_node(ast, step, NO_FLAGS, error);
        if (*error) {
            return FAILURE;    // evaluation failed
        }
        char value[32];
        switch (ast->type_kind) {
            case TKIND_SIGNED:
                sprintf(value, "%" PRId64, res.signed_value);
                break;
            case TKIND_UNSIGNED:
                sprintf(value, "%" PRIu64, res.unsigned_value);
                break;
            case TKIND_FLOAT:
                sprintf(value, "%lf", res.float_value);
                break;
        }
        JSON_NEW_STRING_FIELD(container, "result", value);
        return SUCCESS;
    }

    ULONG ref;
    ULONG pointer_size = 0;
    char *mem = NULL;
    tname = type_name(ast->type_offset, ast->indirect);
    int value_added = 0;
    uint64_t addr, dim, under_kind;

    /* get details of underlying type */
    if (SUCCESS != type_details(ast->type_offset, &dim, &under_kind)) {
        *error = "Cannot get type details";
        RETCLEAN(FAILURE);
    }

    if (!ast->indirect && TKIND_STRUCT == ast->type_kind) {         // struct
        union node_value res = evaluate_node(ast, step, FLAG_ADDR, error);
        if (*error) {
            RETCLEAN(FAILURE);    // evaluation failed
        }
        addr = res.unsigned_value;

        /* process compound variable by adding the reference to it so client can query its internal structure
            in separate request */
        if (SUCCESS != get_var_ref(PTYPE_EXPR, id, "", addr, ast->type_offset, 0, &ref)) {
            RETCLEAN(FAILURE);
        }
        JSON_NEW_INT64_FIELD(container, "variablesReference", ref);
        JSON_NEW_INT64_FIELD(container, "namedVariables", dim);
        JSON_NEW_STRING_FIELD(container, "result", CSTR(tname));
        RETCLEAN(SUCCESS);
    } else {        // pointer or array
        if (ast->indirect) {    // pointer
            /* pointer */
            union node_value res = evaluate_node(ast, step, NO_FLAGS, error);
            if (*error) {
                RETCLEAN(FAILURE);    // evaluation failed
            }
            addr = res.unsigned_value;
            if (0 == addr) {
                JSON_NEW_STRING_FIELD(container, "result", "NULL");
                RETCLEAN(SUCCESS);
            }
            char tmp[32];
            int ret = get_pointer_size(addr, &pointer_size);
            if (MEM_RELEASED == ret) {
                sprintf(tmp, "(%s)0x%" PRIx64 " (dangling)", CSTR(tname), addr);
                JSON_NEW_STRING_FIELD(container, "result", tmp);
                JSON_NEW_INT64_FIELD(container, "variablesReference", 0);
                RETCLEAN(SUCCESS);
            } else if (MEM_NOTFOUND == ret) {
                sprintf(tmp, "(%s)0x%" PRIx64 " (invalid)", CSTR(tname), addr);
                JSON_NEW_STRING_FIELD(container, "result", tmp);
                JSON_NEW_INT64_FIELD(container, "variablesReference", 0);
                RETCLEAN(SUCCESS);
            } else if (SUCCESS != ret) {
                RETCLEAN(FAILURE);
            }
            sprintf(tmp, "(%s)0x%" PRIx64, CSTR(tname), addr);
            JSON_NEW_STRING_FIELD(container, "result", tmp);
            value_added = 1;
            if (!pointer_size) {
                dim = 1;
            } else {
                dim = pointer_size / ast->size;     // number of base type elements in allocated pointer
            }
            // TODO function pointer
        } else {    // array
            union node_value res = evaluate_node(ast, step, FLAG_ADDR, error);
            if (*error) {
                RETCLEAN(FAILURE);    // evaluation failed
            }
            addr = res.unsigned_value;
        }

        /* common code - threat array and pointer in the same way */

        if (ast->size == 1 && (TKIND_SIGNED == under_kind || TKIND_UNSIGNED == under_kind)) {
            /* treat char pointer as string */
            if (!pointer_size) {
                // string of unknown length, get first few characters
                dim = 32;
            }
            /* treat char pointer/array as string */
            mem = get_var_value(addr, dim, cur_step);
            if (!mem) {
                RETCLEAN(FAILURE);
            }
            size_t len = strnlen(mem, dim);
            char *value = malloc(len + 32);
            if (len == dim) {
                /* string is not 0-terminated or longer then limit */
                mem[dim] = '\0';   // mem has one extra byte for terminator
                sprintf(value, "0x%" PRIx64 " \"%s…\"", addr, mem);
            } else {
                sprintf(value, "0x%" PRIx64 " \"%s\"", addr, mem);
            }
            free(mem);
            /* TODO sanitise mem to be a valid JSON string */
            JSON_NEW_STRING_FIELD(container, "result", value);
            JSON_NEW_INT64_FIELD(container, "variablesReference", 0);
            free(value);
            RETCLEAN(SUCCESS);
        }

        if (SUCCESS != get_var_ref(PTYPE_EXPR, id, "", addr, ast->type_offset, ast->indirect, &ref)) {
            RETCLEAN(FAILURE);
        }

        if (!value_added) {      // add value with type, if not added by pointer code above
            JSON_NEW_STRING_FIELD(container, "result", CSTR(tname));
        }
        JSON_NEW_INT64_FIELD(container, "variablesReference", ref);
        JSON_NEW_INT64_FIELD(container, "indexedVariables", dim);
        RETCLEAN(SUCCESS);
    }

cleanup:
    STRFREE(tname);

    return ret;
}


/**************************************************************************
 *
 *  Function:   evaluate_node
 *
 *  Params:     ast - tree/sub-tree to evaluate
 *              step - step to find variable values
 *              flags - NO_FLAGS / FLAG_ADDR
 *              error - where to store error message
 *
 *  Return:     calculated value
 *
 *  Descr:      Recursively evaluate given AST
 *
 **************************************************************************/
union node_value evaluate_node(struct ast_node *ast, uint64_t step, int flags, char **error) {
    uint64_t addr;
    switch (ast->node_type) {
        case NODE_TYPE_INT:
            if (TKIND_SIGNED == ast->type_kind) {
                return (union node_value){ .signed_value = ast->int_value };
            } else {
                return (union node_value){ .unsigned_value = ast->int_value };
            }
        case NODE_TYPE_FLOAT:
            return (union node_value){ .float_value = ast->float_value };
        case NODE_TYPE_STRING:
            return (union node_value){ .pointer_value = ast->str_value };
        case NODE_TYPE_VAR:
            ;
            uint64_t type_offset;
            char *name;
            if (FAILURE == get_var_address(ast->var_id, step, &name, &addr, &type_offset)) {
                *error = "Cannot get variable address";
                return (union node_value){ .pointer_value = NULL };
            }
            if (FLAG_ADDR == flags) {
                return (union node_value){ .unsigned_value = addr};    // parent node needs only address
            }
            return var_value(ast, addr, step, error);
        case NODE_TYPE_FIELD:
            /* find address of struct */
            addr = evaluate_node(ast->object, step, FLAG_ADDR, error).unsigned_value;
            if (*error) {
                return (union node_value){ .pointer_value = NULL };
            }
            addr += ast->start;
            if (FLAG_ADDR == flags) {
                return (union node_value){ .unsigned_value = addr};    // parent node needs only address
            }
            return var_value(ast, addr, step, error);
        case NODE_TYPE_ITEM:
            if (ast->object->indirect) {
                /* find the content of pointer */
                addr = evaluate_node(ast->object, step, NO_FLAGS, error).unsigned_value;
            } else {
                /* find address of array item */
                addr = evaluate_node(ast->object, step, FLAG_ADDR, error).unsigned_value;
            }
            if (*error) {
                return (union node_value){ .pointer_value = NULL };
            }
            uint64_t index = evaluate_node(ast->member, step, FLAG_ADDR, error).unsigned_value;
            addr += index * ast->size;
            if (FLAG_ADDR == flags) {
                return (union node_value){ .unsigned_value = addr};    // parent node needs only address
            }
            return var_value(ast, addr, step, error);
        case NODE_TYPE_UNARY_OP:
            switch (ast->op_code) {
                case OP_ADDR:
                    return evaluate_node(ast->left, step, FLAG_ADDR, error);
                case OP_DEREF:
                    addr = evaluate_node(ast->left, step, NO_FLAGS, error).unsigned_value;
                    if (FLAG_ADDR == flags) {
                        return (union node_value){ .unsigned_value = addr};    // parent node needs only address
                    }
                    return var_value(ast, addr, step, error);
                case OP_NEG:
                    return (union node_value){ .signed_value =
                                    -evaluate_node(ast->left, step, NO_FLAGS, error).signed_value };
                case OP_INV:
                    /* TODO: for inverse op data size is important! */
                    return (union node_value){ .unsigned_value =
                                    ~evaluate_node(ast->left, step, NO_FLAGS, error).unsigned_value };
                case OP_NOT:
                    return (union node_value){ .unsigned_value =
                                    !evaluate_node(ast->left, step, NO_FLAGS, error).unsigned_value };
            }
	    break;	// not really needed, just to silence the warning
        case NODE_TYPE_BINARY_OP:
            if (ast->left->indirect) {
                if (    !(OP_ADD == ast->op_code && TKIND_SIGNED == ast->right->type_kind) &&
                        !!(OP_SUB == ast->op_code && (TKIND_SIGNED == ast->right->type_kind || ast->right->indirect))) {
                    /* should never happen - parser must validate first */
                    *error = "Invalid operator or second operand type for pointer math";
                    return (union node_value){ .pointer_value = NULL };
                }
                char *left = evaluate_node(ast->left, step, NO_FLAGS, error).pointer_value;
                if (*error) {
                    return (union node_value){ .pointer_value = NULL };
                }

                /* pointer +/- integer */
                if (TKIND_SIGNED == ast->right->type_kind) {
                    int64_t offset = evaluate_node(ast->right, step, NO_FLAGS, error).signed_value;
                    if (*error) {
                        return (union node_value){ .pointer_value = NULL };
                    }
                    if (OP_ADD == ast->op_code) {
                        return (union node_value){ .pointer_value = left + offset * ast->left->size };
                    }
                    return (union node_value){ .pointer_value = left - offset * ast->left->size };
                }

                /* pointer - pointer */
                char *right = evaluate_node(ast->right, step, NO_FLAGS, error).pointer_value;
                if (*error) {
                    return (union node_value){ .pointer_value = NULL };
                }
                int64_t diff = left - right;
                if (diff % ast->left->size) {
                    *error = "Pointer difference isn't divisible by pointer size";
                    return (union node_value){ .pointer_value = NULL };
                }
                return (union node_value){ .signed_value = diff /  ast->left->size };
            }

            /* AND and OR are special because support operands of mixed types */
            if (OP_AND == ast->op_code || OP_OR == ast->op_code) {
                int left;
                if (TKIND_FLOAT == ast->left->type_kind) {
                    left = (0 == evaluate_node(ast->left, step, NO_FLAGS, error).float_value);
                } else {
                    left = (0 == evaluate_node(ast->left, step, NO_FLAGS, error).unsigned_value);
                }
                if (*error) {
                    return (union node_value){ .unsigned_value = 0 };
                }
                /* short-circut evaluation */
                if (OP_AND == ast->op_code && !left) {
                    return (union node_value){ .unsigned_value = 0 };
                }
                if (OP_OR == ast->op_code && left) {
                    return (union node_value){ .unsigned_value = 1 };
                }
                int right;
                if (TKIND_FLOAT == ast->right->type_kind) {
                    right = (0 == evaluate_node(ast->right, step, NO_FLAGS, error).float_value);
                } else {
                    right = (0 == evaluate_node(ast->right, step, NO_FLAGS, error).unsigned_value);
                }
                if (*error) {
                    return (union node_value){ .unsigned_value = 0 };
                }
                if (!right) {
                    return (union node_value){ .unsigned_value = 0 };
                }
                return (union node_value){ .unsigned_value = 1 };
            }

            /* for all other ops operand types match */
            if (TKIND_FLOAT == ast->type_kind) {
                /* op with float operands */
                return float_bin_op(ast, step, error);
            } else if (TKIND_SIGNED == ast->type_kind) {
                /* op with signed operands */
                return signed_bin_op(ast, step, error);
            }
            /* op with unsigned operands */
            return unsigned_bin_op(ast, step, error);
        case NODE_TYPE_TYPE:
            ;
            union node_value value = evaluate_node(ast->operand, step, NO_FLAGS, error);
            if (*error) {
                return (union node_value){ .unsigned_value = 0 };
            }
            if (ast->indirect) {
                if (ast->operand->indirect) {
                    return value;
                } else {
                    return (union node_value){ .pointer_value = (void *)value.unsigned_value };
                }
            } else switch (ast->type_kind) {
                case TKIND_UNSIGNED:
                    if (ast->operand->indirect) {
                        return (union node_value){ .unsigned_value = (uint64_t)value.pointer_value };
                    } else if (TKIND_FLOAT == ast->operand->type_kind) {
                        return (union node_value){ .unsigned_value = (uint64_t)value.float_value };
                    }
                    return value;
                case TKIND_SIGNED:
                    if (ast->operand->indirect) {
                        return (union node_value){ .signed_value = (int64_t)value.pointer_value };
                    } else if (TKIND_FLOAT == ast->operand->type_kind) {
                        return (union node_value){ .signed_value = (int64_t)value.float_value };
                    }
                    return value;
                case TKIND_FLOAT:
                    if (TKIND_UNSIGNED == ast->operand->type_kind) {
                        return (union node_value){ .float_value = (double)value.unsigned_value };
                    } else if (TKIND_SIGNED == ast->operand->type_kind) {
                        return (union node_value){ .float_value = (double)value.signed_value };
                    }
                    return value;
            }
            break;
    }

    /* should never get here */
    ERR("Internal error");
    *error = "internal error";
    return (union node_value){ .unsigned_value = 0 };
}


/**************************************************************************
 *
 *  Function:   float_bin_op
 *
 *  Params:     ast - tree/sub-tree to evaluate
 *              step - step to find variable values
 *              error - where to store error message
 *
 *  Return:     calculated value
 *
 *  Descr:      Find the result of operation with float operands, op
 *              result type depends from op
 *
 **************************************************************************/
union node_value float_bin_op(struct ast_node *ast, uint64_t step, char **error) {
    double operand1 = evaluate_node(ast->left, step, NO_FLAGS, error).float_value;
    if (*error) {
        return (union node_value){ .unsigned_value = 0 };
    }
    double operand2 = evaluate_node(ast->right, step, NO_FLAGS, error).float_value;
    if (*error) {
        return (union node_value){ .unsigned_value = 0 };
    }

    switch (ast->op_code) {
        case OP_MUL:
            return (union node_value){ .float_value = operand1 * operand2 };
        case OP_DIV:
            return (union node_value){ .float_value = operand1 / operand2 };
        case OP_ADD:
            return (union node_value){ .float_value = operand1 + operand2 };
        case OP_SUB:
            return (union node_value){ .float_value = operand1 - operand2 };
        /* for relational and logic ops return data type is unsigned */
        case OP_LT:
            return (union node_value){ .unsigned_value = operand1 < operand2 };
        case OP_GT:
            return (union node_value){ .unsigned_value = operand1 > operand2 };
        case OP_LE:
            return (union node_value){ .unsigned_value = operand1 <= operand2 };
        case OP_GE:
            return (union node_value){ .unsigned_value = operand1 >= operand2 };
        case OP_EQ:
            return (union node_value){ .unsigned_value = operand1 == operand2 };
        case OP_NEQ:
            return (union node_value){ .unsigned_value = operand1 != operand2 };
    }

    /* should never get here */
    ERR("Internal error");
    *error = "internal error";
    return (union node_value){ .unsigned_value = 0 };
}


/**************************************************************************
 *
 *  Function:   signed_bin_op
 *
 *  Params:     ast - tree/sub-tree to evaluate
 *              step - step to find variable values
 *              error - where to store error message
 *
 *  Return:     calculated value
 *
 *  Descr:      Find the result of operation with signed operands, op
 *              result type depends from op
 *
 **************************************************************************/
union node_value signed_bin_op(struct ast_node *ast, uint64_t step, char **error) {
    int64_t operand1 = evaluate_node(ast->left, step, NO_FLAGS, error).signed_value;
    if (*error) {
        return (union node_value){ .unsigned_value = 0 };
    }
    int64_t operand2 = evaluate_node(ast->right, step, NO_FLAGS, error).signed_value;
    if (*error) {
        return (union node_value){ .unsigned_value = 0 };
    }

    switch (ast->op_code) {
        case OP_MUL:
            return (union node_value){ .signed_value = operand1 * operand2 };
        case OP_DIV:
            return (union node_value){ .signed_value = operand1 / operand2 };
        case OP_ADD:
            return (union node_value){ .signed_value = operand1 + operand2 };
        case OP_SUB:
            return (union node_value){ .signed_value = operand1 - operand2 };
        case OP_MOD:
            return (union node_value){ .signed_value = operand1 % operand2 };
        /* for relational and logic ops return data type is unsigned */
        case OP_LT:
            return (union node_value){ .unsigned_value = operand1 < operand2 };
        case OP_GT:
            return (union node_value){ .unsigned_value = operand1 > operand2 };
        case OP_LE:
            return (union node_value){ .unsigned_value = operand1 <= operand2 };
        case OP_GE:
            return (union node_value){ .unsigned_value = operand1 >= operand2 };
        case OP_EQ:
            return (union node_value){ .unsigned_value = operand1 == operand2 };
        case OP_NEQ:
            return (union node_value){ .unsigned_value = operand1 != operand2 };
    }

    /* should never get here */
    ERR("Internal error");
    *error = "internal error";
    return (union node_value){ .unsigned_value = 0 };
}


/**************************************************************************
 *
 *  Function:   unsigned_bin_op
 *
 *  Params:     ast - tree/sub-tree to evaluate
 *              step - step to find variable values
 *              error - where to store error message
 *
 *  Return:     calculated value
 *
 *  Descr:      Find the result of operation with unsigned operands, op
 *              result type depends from op
 *
 **************************************************************************/
union node_value unsigned_bin_op(struct ast_node *ast, uint64_t step, char **error) {
    uint64_t operand1 = evaluate_node(ast->left, step, NO_FLAGS, error).unsigned_value;
    if (*error) {
        return (union node_value){ .unsigned_value = 0 };
    }
    uint64_t operand2 = evaluate_node(ast->right, step, NO_FLAGS, error).unsigned_value;
    if (*error) {
        return (union node_value){ .unsigned_value = 0 };
    }

    switch (ast->op_code) {
        case OP_MUL:
            return (union node_value){ .unsigned_value = operand1 * operand2 };
        case OP_DIV:
            return (union node_value){ .unsigned_value = operand1 / operand2 };
        case OP_ADD:
            return (union node_value){ .unsigned_value = operand1 + operand2 };
        case OP_SUB:
            /* for subtraction result type is unsigned */
            return (union node_value){ .signed_value = operand1 - operand2 };
        case OP_LT:
            return (union node_value){ .unsigned_value = operand1 < operand2 };
        case OP_GT:
            return (union node_value){ .unsigned_value = operand1 > operand2 };
        case OP_LE:
            return (union node_value){ .unsigned_value = operand1 <= operand2 };
        case OP_GE:
            return (union node_value){ .unsigned_value = operand1 >= operand2 };
        case OP_EQ:
            return (union node_value){ .unsigned_value = operand1 == operand2 };
        case OP_NEQ:
            return (union node_value){ .unsigned_value = operand1 != operand2 };
        case OP_BIT_AND:
            return (union node_value){ .unsigned_value = operand1 & operand2 };
        case OP_BIT_OR:
            return (union node_value){ .unsigned_value = operand1 | operand2 };
        case OP_XOR:
            return (union node_value){ .unsigned_value = operand1 ^ operand2 };
        case OP_LEFT:
            return (union node_value){ .unsigned_value = operand1 << operand2 };
        case OP_RIGHT:
            return (union node_value){ .unsigned_value = operand1 >> operand2 };
    }

    /* should never get here */
    ERR("Internal error");
    *error = "internal error";
    return (union node_value){ .unsigned_value = 0 };
}


/**************************************************************************
 *
 *  Function:   var_value
 *
 *  Params:     ast - tree/sub-tree to evaluate
 *              addr - variable address
 *              step - step to find variable values
 *              error - where to store error message
 *
 *  Return:     found variable value
 *
 *  Descr:      Get variable value by address and cast it to required
 *              data type
 *
 **************************************************************************/
union node_value var_value(struct ast_node *ast, uint64_t addr, uint64_t step, char **error) {
    char *value = get_var_value(addr, ast->size, step);
    if (!value) {
        *error = "Cannot get variable value";
        return (union node_value){ .pointer_value = NULL };
    }
    union node_value ret;
    if (ast->indirect) {    // pointer address
        ret.unsigned_value = *(uint64_t *)value;
        free(value);
        return ret;
    }
    switch (ast->type_kind) {
        case TKIND_SIGNED:
            switch (ast->size) {
                case sizeof(int8_t):
                    ret.signed_value = *(int8_t *)value;
                    break;
                case sizeof(int16_t):
                    ret.signed_value = *(int16_t *)value;
                    break;
                case sizeof(int32_t):
                    ret.signed_value = *(int32_t *)value;
                    break;
                case sizeof(int64_t):
                    ret.signed_value = *(int64_t *)value;
                    break;
                default:
                    *error = "Unsupported size for signed type";
                    break;
            }
            break;
        case TKIND_UNSIGNED:
            switch (ast->size) {
                case sizeof(uint8_t):
                    ret.unsigned_value = *(uint8_t *)value;
                    break;
                case sizeof(uint16_t):
                    ret.unsigned_value = *(uint16_t *)value;
                    break;
                case sizeof(uint32_t):
                    ret.unsigned_value = *(uint32_t *)value;
                    break;
                case sizeof(uint64_t):
                    ret.unsigned_value = *(uint64_t *)value;
                    break;
                default:
                    *error = "Unsupported size for unsigned type";
                    break;
            }
            break;
        case TKIND_FLOAT:
            switch (ast->size) {
                case sizeof(float):
                    ret.float_value = *(float *)value;
                    break;
                case sizeof(double):
                    ret.float_value = *(double *)value;
                    break;
#if __SIZEOF_LONG_DOUBLE__ > __SIZEOF_DOUBLE__      // to avoid compile error with equal cases
                case __SIZEOF_LONG_DOUBLE__:
                    ret.float_value = (double)*(long double *)value;    // loosing precision here!
                    break;
#endif
                default:
                    *error = "Unsupported size for float type";
                    break;
            }
            break;
        default:
            *error = "Unsupported type for variable";
    }
    free(value);
    if (*error) {
        return (union node_value){ .pointer_value = NULL };
    }
    return ret;
}


/**************************************************************************
 *
 *  Function:   new_ast_node
 *
 *  Params:     type - type of node to create
 *
 *  Return:     newely-created node
 *
 *  Descr:      Create new AST node of specified type
 *
 **************************************************************************/
struct ast_node *new_ast_node(int type) {
    struct ast_node *node = calloc(1, sizeof(*node));
    node->node_type = type;

    return node;
}


/**************************************************************************
 *
 *  Function:   free_ast_node
 *
 *  Params:     node - node to destroy
 *
 *  Return:     N/A
 *
 *  Descr:      Destory node, all its sub-nodes and other allocated memory
 *
 **************************************************************************/
void free_ast_node(struct ast_node *node) {
    if (!node) {
        return;
    }

    switch (node->node_type) {
        case NODE_TYPE_STRING:
            free(node->str_value);
            break;
        case NODE_TYPE_ITEM:
            /* FALLTHROUGH */
        case NODE_TYPE_FIELD:
            free_ast_node(node->object);
            free_ast_node(node->member);
            break;
        case NODE_TYPE_VAR:
//            free(node->var_name);
            break;
        case NODE_TYPE_UNARY_OP:
            free_ast_node(node->left);
            /* FALLTHROUGH */
        case NODE_TYPE_BINARY_OP:
            free_ast_node(node->right);
            break;
        case NODE_TYPE_TYPE:
            free_ast_node(node->operand);
            break;
    }
    free(node);
}


/**************************************************************************
 *
 *  Function:   get_struct_details
 *
 *  Params:     name - name to look for
 *              offset - where to store type offset
 *              kind - where to store type kind
 *              size - where to store size
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find details for given struct/union
 *
 **************************************************************************/
int get_struct_details(const char *name, uint64_t *offset, int *kind, size_t *size) {
    if (!expr_struct_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&expr_struct_cursor,
            "SELECT "
                "offset, "
                "flags & " STR(TKIND_TYPE) ", "
                "size "
            "FROM "
                "type "
            "WHERE "
                "flags & " STR(TKIND_TYPE) " IN (" STR(TKIND_STRUCT) ", " STR(TKIND_UNION) ") AND "
                "name = ?",
            name
        )) {
            ERR("Cannot prepare struct cursor");
            return FAILURE;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(expr_struct_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_struct_cursor, name)) {
        ERR("Cannot bind struct cursor");
        return FAILURE;
    }

    int ret = DAB_CURSOR_FETCH(expr_struct_cursor, offset, kind, size);
    if (DAB_OK != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_type_details
 *
 *  Params:     name - name to look for
 *              offset - where to store type offset
 *              kind - where to store type kind
 *              size - where to store size
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find details for given non-struct/union type
 *
 **************************************************************************/
int get_type_details(const char *name, uint64_t *offset, int *kind, size_t *size) {
    if (!expr_type_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&expr_type_cursor,
            "SELECT "
                "offset, "
                "flags & " STR(TKIND_TYPE) ", "
                "size "
            "FROM "
                "type "
            "WHERE "
                "flags & " STR(TKIND_TYPE) " NOT IN (" STR(TKIND_STRUCT) ", " STR(TKIND_UNION) ") AND "
                "name = ?",
            name
        )) {
            ERR("Cannot prepare type cursor");
            return FAILURE;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(expr_type_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_type_cursor, name)) {
        ERR("Cannot bind type cursor");
        return FAILURE;
    }

    int ret = DAB_CURSOR_FETCH(expr_type_cursor, offset, kind, size);
    if (DAB_OK != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_var_details
 *
 *  Params:     name - name to look for
 *              scope - of the var
 *              var_offset - where to store var offset
 *              type_offset - where to store type offset
 *              kind - where to store type kind
 *              size - where to store size
 *              indirect - where to store number of indirections
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find details for given variable (within given scope)
 *
 *  Note:       Return info about basic type of the variable, e.g. if
 *              variable is a pointer, return the type it points to
 *
 **************************************************************************/
int get_var_details(const char *name, uint64_t scope, uint64_t *var_id,
                uint64_t *type_offset, int *kind, size_t *size, int *indirect) {
    if (!expr_var_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&expr_var_cursor,
            "SELECT "
                "v.id, "
                "a.offset, "
                "a.flags & " STR(TKIND_TYPE) ", "
                "a.size, "
                "t.indirect "
            "FROM "
                "var v "
                "JOIN type t ON t.offset = v.type_offset "
                "JOIN type_relation tr ON "
                    "tr.descendant = t.offset "
                "JOIN type a ON "
                    "a.offset = tr.ancestor "
            "WHERE "
                "(v.scope_id = ? OR "
                    "v.scope_id IN (SELECT ancestor FROM scope_ancestor WHERE id = ?)"
                ") AND "
                "a.indirect = 0 AND "
                "v.name = ? "
            "ORDER BY "
                "depth",
            scope, scope, name
        )) {
            ERR("Cannot prepare var cursor");
            return FAILURE;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(expr_var_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_var_cursor, scope, scope, name)) {
        ERR("Cannot bind var cursor");
        return FAILURE;
    }

    int ret = DAB_CURSOR_FETCH(expr_var_cursor, var_id, type_offset, kind, size, indirect);
    if (DAB_OK != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_field_details
 *
 *  Params:     name - name to look for
 *              type - type offset of parent struct/union
 *              type_offset - where to store type offset
 *              kind - where to store type kind
 *              size - where to store size
 *              start - where to store field offset within struct
 *              indirect - where to store number of indirections
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find details for given field (within given struct)
 *
 *  Note:       Return info about basic type of the field, e.g. if
 *              field is a pointer, return the type it points to
 *
 **************************************************************************/
int get_field_details(const char *name, uint64_t type, uint64_t *type_offset,
            int *kind, size_t *size, uint64_t *start, int *indirect) {
    if (!expr_field_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&expr_field_cursor,
            "SELECT "
                "a.offset, "
                "a.flags & " STR(TKIND_TYPE) ", "
                "a.size, "
                "m.start, "
                "t.indirect "
            "FROM "
                "member m "
                "JOIN type t ON "
                    "t.offset = m.type "
                "JOIN type_relation ON "
                    "descendant = t.offset "
                "JOIN type a ON "
                    "a.offset = ancestor "
            "WHERE "
                "m.offset = ? AND "
                "m.name = ? "
            "ORDER BY "
                "depth DESC",
            type, name
        )) {
            ERR("Cannot prepare field cursor");
            return FAILURE;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(expr_field_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_field_cursor, type, name)) {
        ERR("Cannot bind field cursor");
        return FAILURE;
    }

    int ret = DAB_CURSOR_FETCH(expr_field_cursor, type_offset, kind, size, start, indirect);
    if (DAB_OK != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_base_type_details
 *
 *  Params:     offset - offset of type to lookup
 *              type_offset - where to store type offset
 *              kind - where to store type kind
 *              size - where to store size
 *              indirect - where to store number of indirections
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find details for base type (typically used for arrays)
 *
 *  Note:       Return info about the most base type of the type
 *
 **************************************************************************/
int get_base_type_details(uint64_t offset, uint64_t *type_offset, int *kind, size_t *size, int *indirect) {
    if (!expr_basetype_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&expr_basetype_cursor,
            "SELECT "
                "a.offset, "
                "a.flags & " STR(TKIND_TYPE) ", "
                "a.size, "
                "t.indirect "
            "FROM "
                "type t "
                "JOIN type_relation ON "
                    "descendant = t.offset "
                "JOIN type a ON "
                    "a.offset = ancestor "
            "WHERE "
                "t.offset = ? AND "
                "depth > 0 "        // just a pre-caution not to get the type itself
            "ORDER BY "
                "depth DESC",
            offset
        )) {
            ERR("Cannot prepare base type cursor");
            return FAILURE;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(expr_basetype_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_basetype_cursor, offset)) {
        ERR("Cannot bind base type cursor");
        return FAILURE;
    }

    int ret = DAB_CURSOR_FETCH(expr_basetype_cursor, type_offset, kind, size, indirect);
    if (DAB_OK != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_expr_id
 *
 *  Params:     expr_text - text of the expression
 *              id - where to store found/added ID
 *              ast - where to store address of found AST / NULL is added
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Return expression ID by text
 *
 *  Note:       If expression is cached, also return the associated AST
 *
 **************************************************************************/
int query_expr_cache(const char *expr_text, uint64_t *id, struct ast_node **ast) {
    uint64_t addr;

    if (!expr_addexpr_cursor) {
        /* create in-memory table for var references */
        if (DAB_OK != DAB_EXEC("CREATE TABLE local.expr ("
                "id             INTEGER PRIMARY KEY AUTOINCREMENT, "
                "expr_text      VARCHAR UNIQUE NOT NULL, "
                "ast            INTEGER"
            ")"
        )) {
            ERR("Cannot create in-memory table");
            return FAILURE;
        }

        if (DAB_OK != DAB_CURSOR_PREPARE(&expr_addexpr_cursor, "INSERT INTO local.expr "
                "(expr_text) "
                "VALUES "
                "(?)"
        )) {
            ERR("Cannot prepare expr cache insert statement");
            return FAILURE;
        }

        if (DAB_OK != DAB_CURSOR_PREPARE(&expr_getexpr_cursor, "SELECT  "
                "id, "
                "ast "
            "FROM "
                "local.expr "
            "WHERE "
                "expr_text = ?"
        )) {
            ERR("Cannot prepare expr cache query");
            return FAILURE;
        }
    }

    /* try to find expression in cache */
    if ( DAB_OK != DAB_CURSOR_RESET(expr_getexpr_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_getexpr_cursor, expr_text)) {
        ERR("Cannot bind expr cache query cursor");
        return FAILURE;
    }
    int ret = DAB_CURSOR_FETCH(expr_getexpr_cursor, id, &addr);
    if (DAB_OK == ret) {
        *ast = (struct ast_node *)addr;
        return SUCCESS;
    } else if (DAB_NO_DATA != ret) {
        return FAILURE;
    }

    /* expression not found - add it */
    if ( DAB_OK != DAB_CURSOR_RESET(expr_addexpr_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_addexpr_cursor, expr_text)) {
        ERR("Cannot bind expr cache query cursor");
        return FAILURE;
    }
    ret = DAB_CURSOR_FETCH(expr_addexpr_cursor);
    if (DAB_NO_DATA != ret) {
        return FAILURE;
    }
    *id = DAB_LAST_ID;
    *ast = NULL;
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   update_expr_cache
 *
 *  Params:     id - ID of expression to update
 *              ast - AST address
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Return expression ID by text
 *
 *  Note:       If expression is cached, also return the associated AST
 *
 **************************************************************************/
int update_expr_cache(uint64_t id, struct ast_node *ast) {
    if (!expr_updexpr_cursor) {
        if (DAB_OK != DAB_CURSOR_PREPARE(&expr_updexpr_cursor, "UPDATE local.expr "
                "SET ast = ? "
            "WHERE "
                "id = ?"
        )) {
            ERR("Cannot prepare update expr cache statement");
            return FAILURE;
        }
    }

    if ( DAB_OK != DAB_CURSOR_RESET(expr_updexpr_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_updexpr_cursor, id, (uint64_t)ast)) {
        ERR("Cannot bind expr cache update cursor");
        return FAILURE;
    }
    int ret = DAB_CURSOR_FETCH(expr_updexpr_cursor);
    if (DAB_NO_DATA != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   type_details
 *
 *  Params:     type_offset
 *              dim - where to store array dimension/field count
 *              type_kind - where to store base type's kind
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find details about type
 *
 **************************************************************************/
int type_details(uint64_t type_offset, uint64_t *dim, uint64_t *type_kind) {
    if (!expr_typedetails_cursor) {
        if (DAB_OK != DAB_CURSOR_PREPARE(&expr_typedetails_cursor, "SELECT "
                "t.dim, "
                "p.flags & " STR(TKIND_TYPE) " "
            "FROM "
                "type t "
                "LEFT JOIN type p ON p.offset = t.parent "
            "WHERE "
                "t.offset  = ?"
        )) {
            ERR("Cannot prepare type details query");
            return FAILURE;
        }
    }

    if ( DAB_OK != DAB_CURSOR_RESET(expr_typedetails_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(expr_typedetails_cursor, type_offset)) {
        ERR("Cannot bind expr cache update cursor");
        return FAILURE;
    }
    int ret = DAB_CURSOR_FETCH(expr_typedetails_cursor, dim, type_kind);
    if (DAB_OK != ret) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   close_expr_cursors
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Close all cursors, deallocate ASTs for all cached expressions
 *
 **************************************************************************/
void close_expr_cursors(void) {
    if (expr_addexpr_cursor) {
        void *ast_cache;
        if (DAB_OK != DAB_CURSOR_PREPARE(&ast_cache, "SELECT "
                "ast "
            "FROM "
                "local.expr"
        )) {
            return;
        }
        uint64_t addr;
        while (DAB_OK == DAB_CURSOR_FETCH(ast_cache, &addr)) {
            free_ast_node((struct ast_node *)addr);
        }
        DAB_CURSOR_FREE(ast_cache);
    }
    DAB_CURSOR_FREE(expr_struct_cursor);
    DAB_CURSOR_FREE(expr_type_cursor);
    DAB_CURSOR_FREE(expr_var_cursor);
    DAB_CURSOR_FREE(expr_field_cursor);
    DAB_CURSOR_FREE(expr_basetype_cursor);
    DAB_CURSOR_FREE(expr_addexpr_cursor);
    DAB_CURSOR_FREE(expr_getexpr_cursor);
    DAB_CURSOR_FREE(expr_updexpr_cursor);
    DAB_CURSOR_FREE(expr_typedetails_cursor);
}

