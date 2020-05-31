/**************************************************************************
 *
 *  File:       vars.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      All variable-related functions for 'examine'
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
#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/user.h>
#include <libdwarf/libdwarf.h>
#include <libdwarf/dwarf.h>

#include <stingray.h>
#include <eel.h>
#include <dab.h>

#include "flightrec.h"
#include "examine.h"
#include "mem.h"
#include "jsonapi.h"

static Dwarf_Debug dbg;

void *var_cursor;
void *step_cursor;
void *array_cursor;
void *struct_cursor;
void *member_cursor;
void *mem_cursor;
void *type_cursor;
void *ref_cursor;
void *ref_upsert;
void *heap_cursor;
void *func_cursor;
void *type_name_cursor;
void *enum_cursor;

/* DWARF uses its own register numbering scheme.
   See https://www.uclibc.org/docs/psABI-x86_64.pdf fig 3.36 for DWARF - x86_64 register mapping */
int dwarf_registers[] = {                   // support only first 16 registers for now
    offsetof(struct user_regs_struct, rax),
    offsetof(struct user_regs_struct, rdx),
    offsetof(struct user_regs_struct, rcx),
    offsetof(struct user_regs_struct, rbx),
    offsetof(struct user_regs_struct, rsi),
    offsetof(struct user_regs_struct, rdi),
    offsetof(struct user_regs_struct, rbp),
    offsetof(struct user_regs_struct, rsp),
    offsetof(struct user_regs_struct, r8),
    offsetof(struct user_regs_struct, r9),
    offsetof(struct user_regs_struct, r10),
    offsetof(struct user_regs_struct, r11),
    offsetof(struct user_regs_struct, r12),
    offsetof(struct user_regs_struct, r13),
    offsetof(struct user_regs_struct, r14),
    offsetof(struct user_regs_struct, r15),
};

static int get_location(Dwarf_Attribute attrib, REG_TYPE pc, LLONG base_addr, struct user_regs_struct *regs,
                        LLONG *address);
static int add_var_entry(JSON_OBJ *container, int parent_type, ULONG parent, char *name, ULONG addr,
                        ULONG type, int indirect);
static int func_name(ULONG address, char **name);


/**************************************************************************
 *
 *  Function:   open_dbginfo
 *
 *  Params:     filename
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Open binary for reading debug info
 *
 **************************************************************************/
int open_dbginfo(const char *filename) {
    Dwarf_Error err = NULL;
    int fd;
    int ret = SUCCESS;

    fd = open(filename, O_RDONLY);
    if (!fd) {
        ERR("Cannot open %s - %s", filename, strerror(errno));
        return FAILURE;
    }

    ret = dwarf_init(fd, DW_DLC_READ, NULL, NULL, &dbg, &err);
    if (DW_DLV_ERROR == ret) {
        ERR("DWARF init failed - %s", dwarf_errmsg(err));
        return FAILURE;
    } else if (DW_DLV_NO_ENTRY == ret) {
        ERR("No DWARF information found");
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   add_var
 *
 *  Params:     scope - scope for the var
 *              container - JSON object to add var details to
 *              var_id - ID of var
 *              step - step to get var value for
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find variable location and add var entry to JSON container
 *
 **************************************************************************/
int add_var(ULONG scope, JSON_OBJ *container, ULONG var_id, ULONG step) {
    char *name;
    uint64_t addr, type_offset;

    if (FAILURE == get_var_address(var_id, step, &name, &addr, &type_offset)) {
        return FAILURE;
    }

    // add entry to JSON
    if (FAILURE == add_var_entry(container, PTYPE_SCOPE, scope, name, addr, type_offset, 0)) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_var_address
 *
 *  Params:     var_id
 *              step - step to get var address for
 *              name - where to store var name
 *              addr - where to store var address
 *              type_offset - where to store type offset
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Get variable address for specified step, and other details
 *
 **************************************************************************/
int get_var_address(uint64_t var_id, uint64_t step, char **name, uint64_t *address, uint64_t *type_offset) {
    Dwarf_Error err = NULL;
    Dwarf_Die die = NULL;
    int ret= SUCCESS;

    /* find var details by ID */
    if (!var_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&var_cursor,
            "SELECT "
                "v.name, "
                "v.offset, "
                "f.offset, "
                "u.base_addr, "
                "v.type_offset "
            "FROM "
                "var v "
                "JOIN unit u ON "
                    "u.id = v.unit_id "
                "LEFT JOIN func_for_scope f ON "        // need LEFT JOIN for global vars
                    "f.scope_id = v.scope_id "
            "WHERE "
                "v.id = ?", var_id
        )) {
            return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(var_cursor) || DAB_OK != DAB_CURSOR_BIND(var_cursor, var_id)) {
        return FAILURE;
    }
    Dwarf_Off var_offset, func_offset;
    LLONG base;
    if (DAB_OK != DAB_CURSOR_FETCH(var_cursor, name, &var_offset, &func_offset, &base, type_offset)) {
        ERR("Cannot find details for variable %" PRIu64, var_id);
        return FAILURE;
    }

    /* get registers (especially PC) for the step */
    if (!step_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&step_cursor,
            "SELECT "
                "regs "
            "FROM "
                "step "
            "WHERE "
                "id = ?", step
        )) {
            return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(step_cursor) || DAB_OK != DAB_CURSOR_BIND(step_cursor, step)) {
        return FAILURE;
    }

    /* manualy assemble Stingray string to be used as BLOB */
    struct sr *registers = sr_new("", sizeof(struct user_regs_struct) + 1);
    if (DAB_OK != DAB_CURSOR_FETCH(step_cursor, registers)) {
        ERR("Cannot find registers for step %" PRIu64, step);
        RETCLEAN(FAILURE);
    }
    struct user_regs_struct regs;
    memcpy(&regs, CSTR(registers), sizeof(regs));

    /* find variable location information */
    int dwarf_ret = dwarf_offdie_b(dbg, var_offset, 1, &die, &err);
    if (DW_DLV_ERROR == dwarf_ret) {
        ERR("Cannot find debug entry for offset x%llx - %s", var_offset, dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    } else if (DW_DLV_NO_ENTRY == dwarf_ret) {
        ERR("No DWARF entry found for offset x%llx", var_offset);
        RETCLEAN(FAILURE);
    }
    Dwarf_Attribute attrib;
    if (DW_DLV_ERROR == dwarf_attr(die, DW_AT_location, &attrib, &err)) {
        ERR("Getting DW_AT_location for offset x%llx", var_offset);
        RETCLEAN(FAILURE);
    }
    LLONG addr;   // signed because can be negative for relative address
    if (SUCCESS != get_location(attrib, regs.rip, base, &regs, &addr)) {
        RETCLEAN(FAILURE);          // cannot get variable location
    }
    /* negative address as offset from frame base - find frame base from function DIE */
    if (addr < 0) {
        dwarf_dealloc(dbg, die, DW_DLA_DIE);
        dwarf_ret = dwarf_offdie_b(dbg, func_offset, 1, &die, &err);
        if (DW_DLV_ERROR == dwarf_ret) {
            ERR("Cannot find debug entry for offset x%llx - %s", var_offset, dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        } else if (DW_DLV_NO_ENTRY == dwarf_ret) {
            ERR("No DWARF entry found for offset x%llx", var_offset);
            RETCLEAN(FAILURE);
        }
        Dwarf_Attribute attrib;
        if (DW_DLV_ERROR == dwarf_attr(die, DW_AT_frame_base, &attrib, &err)) {
            ERR("Getting DW_AT_location for offset x%llx", var_offset);
            RETCLEAN(FAILURE);
        }
        LLONG frame_base;
        if (SUCCESS != get_location(attrib, regs.rip, base, &regs, &frame_base)) {
            RETCLEAN(FAILURE);
        }
        addr += frame_base;
    }
    *address = (uint64_t)addr;

cleanup:
    STRFREE(registers);
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }
    if (die) {
        dwarf_dealloc(dbg, die, DW_DLA_DIE);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   add_var_items
 *
 *  Params:     container - JSON object to add var details to
 *              var_id - ID of var
 *              start - item to start from
 *              count - number of items to process
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Add array items to JSON object
 *
 **************************************************************************/
int add_var_items(JSON_OBJ *container, ULONG ref_id, unsigned int start, unsigned int count) {
    if (!array_cursor) {
        if (DAB_OK != DAB_CURSOR_PREPARE(&array_cursor, "SELECT "
                "ref.address, "
                "IFNULL(base.offset, type.offset), "
                "IFNULL(base.size, type.size), "
                "type.dim, "
                "ref.indirect "
            "FROM "
                "local.ref ref "
                "JOIN type ON type.offset = ref.type "
                "LEFT JOIN type base ON base.offset = type.parent "
            "WHERE "
                "ref.id = ?"
        )) {
            return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(array_cursor)) {
        return FAILURE;
    }
    if (DAB_OK != DAB_CURSOR_BIND(array_cursor, ref_id)) {
        return FAILURE;
    }
    ULONG address, item_size, dim, base_type;
    int indirect;
    if (DAB_OK != DAB_CURSOR_FETCH(array_cursor, &address, &base_type, &item_size, &dim, &indirect)) {
        return FAILURE;
    }
    if (!dim || indirect) {
        /* var is pointer - try to get size */
        ULONG pointer_size;
        if (SUCCESS != get_pointer_size(address, &pointer_size)) {
            return FAILURE;
        }
        if (!pointer_size) {
            dim = 1;
        } else {
            dim = pointer_size / item_size;
        }
    }
    char name[32];
    for (unsigned int i = start; i < count && i < dim; i++) {
        sprintf(name, "[%d]", i);
        if (FAILURE == add_var_entry(container, 1, ref_id, name, address + i * item_size, base_type,
                                        indirect ? indirect-1 : 0)) {
            return FAILURE;
        }
    }
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   add_var_fields
 *
 *  Params:     container - JSON object to add var details to
 *              ref_id - variable reference
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Add struct fields to JSON object
 *
 **************************************************************************/
int add_var_fields(JSON_OBJ *container, ULONG ref_id) {
    if (!struct_cursor) {
        if (DAB_OK != DAB_CURSOR_PREPARE(&struct_cursor, "SELECT "
                "address, "
                "type "
            "FROM "
                "local.ref "
            "WHERE "
                "id = ?"
        )) {
            return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(struct_cursor)) {
        return FAILURE;
    }
    if (DAB_OK != DAB_CURSOR_BIND(struct_cursor, ref_id)) {
        return FAILURE;
    }
    ULONG address, type;
    if (DAB_OK != DAB_CURSOR_FETCH(struct_cursor, &address, &type)) {
        return FAILURE;
    }

    if (!member_cursor) {
        if (DAB_OK != DAB_CURSOR_PREPARE(&member_cursor, "SELECT "
                "name, "
                "start, "
                "type "
            "FROM "
                "member "
            "WHERE "
                "offset = ? "
            "ORDER BY "
                "start"
        )) {
            return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(member_cursor)) {
        return FAILURE;
    }
    if (DAB_OK != DAB_CURSOR_BIND(member_cursor, type)) {
        return FAILURE;
    }

    /* loop through members and add entries */
    int ret;
    char *name;
    ULONG start_offset;
    while (DAB_OK == (ret = DAB_CURSOR_FETCH(member_cursor, &name, &start_offset, &type))) {
        if (FAILURE == add_var_entry(container, 1, ref_id, name, address + start_offset, type, 0)) {
            return FAILURE;
        }
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_location
 *
 *  Params:     attrib - DWARF attribute containing location
 *              pc - program counter
 *              base_addr - base address to calculate offsets from
 *              regs - CPU registers (may be needed for calculating location)
 *              address - where to store address. Negative value means
 *                        offset from frame base, positive - absolute address
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Find location of variable, for given value of PC
 * 
 *  Note:       base_addr, passed as param, is unit base address, and it
 *              doesn't necessarely include program base address, so it
 *              is added here
 *
 **************************************************************************/
int get_location(Dwarf_Attribute attrib, REG_TYPE pc, LLONG base_addr, struct user_regs_struct *regs, LLONG *address) {
    Dwarf_Unsigned count;
    Dwarf_Loc_Head_c head = NULL;
    Dwarf_Error err = NULL;
    int ret = SUCCESS;

    if (DW_DLV_OK != dwarf_get_loclist_c(attrib, &head, &count, &err)) {
        ERR("Getting location information failed - %s", dwarf_errmsg(err));
        return FAILURE;
    }

    for (Dwarf_Unsigned i = 0; i < count; i++) {
        Dwarf_Small lle_value, list_source;
        Dwarf_Addr lo_pc, hi_pc;
        Dwarf_Locdesc_c entry;
        Dwarf_Unsigned  op_count, expr_offset, locdesc_offset;
        if (DW_DLV_OK != dwarf_get_locdesc_entry_c(
                head,
                i,
                &lle_value,
                &lo_pc,
                &hi_pc,
                &op_count,
                &entry,
                &list_source,
                &expr_offset,
                &locdesc_offset,
                &err)) {
            ERR("Getting location information failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }

        // TODO: support other possible lle_value
        if (DW_LLEX_offset_pair_entry == lle_value) {
            lo_pc += base_addr + program_base_addr;
            hi_pc += base_addr + program_base_addr;
        }

        switch (list_source) {
            case 1: // DWARF2/3/4 location entry
                    if (pc < lo_pc || pc >= hi_pc) {
                        continue;   // loclist entry doesn't apply to current value of PC - try next
                    }
                    /* FALLTHROUGH */
            case 0: // expression
                    ;   // empty statement is needed because var declaration cannot have a label
                    Dwarf_Small op;
                    Dwarf_Unsigned opd1, opd2, opd3, off;
                    // TODO: Make support for complex location lists with op_count > 1
                    if (DW_DLV_OK != dwarf_get_location_op_value_c(entry, 0, &op, &opd1, &opd2, &opd3, &off, &err)) {
                        ERR("Getting location value failed - %s", dwarf_errmsg(err));
                        RETCLEAN(FAILURE);
                    }
                    switch (op) {
                        case DW_OP_addr:            // absolute address
                            *address = (LLONG)opd1 + program_base_addr;
                            break;
                        case DW_OP_fbreg:           // address relative to frame base, signed
                            *address = (LLONG)opd1;     // opd1 is unsigned but holds a negative signed value,
                                                        // so casting to signed type fixes it
                            break;
                        case DW_OP_call_frame_cfa:
                            ERR("DW_OP_call_frame_cfa is not supported");
                            RETCLEAN(FAILURE);
                        case DW_OP_breg0 ... DW_OP_breg15:
                            // register content + offset
                            *address = (*(REG_TYPE *)(((char *)regs) + dwarf_registers[op-DW_OP_breg0])) + opd1;
                            break;
                        default:
                            ERR("Unsupported opcode 0x%x for location expression", op);
                            RETCLEAN(FAILURE);
                    }
                    break;
            case 2: /* DWARF5 split location entry */
                    break;
        }
    }


cleanup:
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }
    if (head) {
        dwarf_loc_head_c_dealloc(head);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   get_var_value
 *
 *  Params:     addr - address to get value from
 *              size - how many bytes to read
 *              step - program step to get value for
 *
 *  Return:     allocated buffer (need to be freed) / NULL on error
 *
 *  Descr:      Get memory content for the specified address
 *
 **************************************************************************/
char *get_var_value(ULONG addr, size_t size, uint64_t step) {
    /* find memory content */
    if (!mem_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&mem_cursor,
            "SELECT "
                "address, "
                "content "
            "FROM "
                "mem "
            "WHERE "
                "step_id <= ? AND "
                "address <= ? AND "
                "address >= ? "
            "GROUP BY "
                "address "
            "HAVING "
                "step_id = MAX(step_id)",
                step,
                addr + size,
                addr - MEM_SEGMENT_SIZE
        )) {
            return NULL;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(mem_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(mem_cursor, step, addr+size, addr-MEM_SEGMENT_SIZE)) {
        return NULL;
    }

    ULONG chunk_start;
    struct sr *content = sr_new("", MEM_SEGMENT_SIZE + 1);
    int ret = SUCCESS;
    char *buffer = calloc(size + 1, 1);     // extra byte for 0 termination, if needed
    char *cur = buffer;
    while (DAB_OK == (ret = DAB_CURSOR_FETCH(mem_cursor, &chunk_start, content))) {
        size_t to_copy = MEM_SEGMENT_SIZE;
        char *from = CSTR(content);
        if (chunk_start + MEM_SEGMENT_SIZE > addr + size) {
            to_copy -= (chunk_start + MEM_SEGMENT_SIZE) - (addr + size);
        }
        if (chunk_start < addr) {
            to_copy -= addr - chunk_start;
            from += addr - chunk_start;
        }
        memcpy(cur, from, to_copy);
        cur += to_copy;
    }
    STRFREE(content);
    if (DAB_NO_DATA != ret) {
        free(buffer);
        return NULL;
    }

    return buffer;
}


/**************************************************************************
 *
 *  Function:   add_var_entry
 *
 *  Params:     container - JSON object to add var details to
 *              parent_type - type of parent reference, one of PTYPE_XXX
 *              parent - parent reference
 *              name - var name
 *              addr - var address
 *              type - type offset
 *
 *  Return:     allocated buffer (need to be freed) / NULL on error
 *
 *  Descr:      Get memory content for the specified address
 *
 **************************************************************************/
int add_var_entry(JSON_OBJ *container, int parent_type, ULONG parent, char *name, ULONG addr,
                    ULONG type, int indirect) {
    char *mem = NULL;
    int ret = SUCCESS;

    if (!type_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&type_cursor,
            "SELECT "
                "size, "
                "dim, "
                "flags, "
                "parent "
            "FROM "
                "type "
            "WHERE "
                "offset = ?", type
        )) {
            return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(type_cursor) || DAB_OK != DAB_CURSOR_BIND(type_cursor, type)) {
        return FAILURE;
    }

    ULONG size, flags, dim, base_type;
    if (DAB_OK != DAB_CURSOR_FETCH(type_cursor, &size, &dim, &flags, &base_type)) {
        ERR("Cannot find type with offset %lx", type);
        return FAILURE;
    }

    JSON_OBJ *item = JSON_ADD_NEW_ITEM(container);
    JSON_NEW_STRING_FIELD(item, "name", name);

    /* format variable value for basic types */
    char new_val[64];
    ULONG ref;
    ULONG pointer_size = 0;
    ULONG dummy;
    struct sr *tname;
    tname = type_name(type, indirect);
    int value_added = 0;

    // hack to force pointer processing
    int org_flags = flags;
    if (indirect) {
        flags = TKIND_POINTER;
    }
    switch (flags & TKIND_TYPE) {
        case TKIND_STRUCT:  /* FALLTHROUGH */
        case TKIND_UNION:
            /* process compound variable by adding the reference to it so client can query its internal structure
                in separate request */
            if (SUCCESS != get_var_ref(parent_type, parent, name, addr, type, 0, &ref)) {
                RETCLEAN(FAILURE);
            }
            JSON_NEW_INT64_FIELD(item, "variablesReference", ref);
            JSON_NEW_INT64_FIELD(item, "namedVariables", dim);
            JSON_NEW_STRING_FIELD(item, "value", CSTR(tname));
            RETCLEAN(SUCCESS);
        case TKIND_POINTER:
            flags = org_flags;
            mem = get_var_value(addr, size, cur_step);
            if (!mem) {
                RETCLEAN(FAILURE);
            }
            addr = *(ULONG *)mem;
            free(mem);
            if (0 == addr) {
                JSON_NEW_STRING_FIELD(item, "value", "NULL");
                JSON_NEW_INT64_FIELD(item, "variablesReference", 0);
                RETCLEAN(SUCCESS);
            }
            char tmp[32];
            int ret = get_pointer_size(addr, &pointer_size);
            if (MEM_RELEASED == ret) {
                sprintf(tmp, "(%s)0x%" PRIx64 " (dangling)", CSTR(tname), addr);
                JSON_NEW_STRING_FIELD(item, "value", tmp);
                JSON_NEW_INT64_FIELD(item, "variablesReference", 0);
                RETCLEAN(SUCCESS);
            } else if (MEM_NOTFOUND == ret) {
                sprintf(tmp, "(%s)0x%" PRIx64 " (invalid)", CSTR(tname), addr);
                JSON_NEW_STRING_FIELD(item, "value", tmp);
                JSON_NEW_INT64_FIELD(item, "variablesReference", 0);
                RETCLEAN(SUCCESS);
            } else if (SUCCESS != ret) {
                RETCLEAN(FAILURE);
            }
            sprintf(tmp, "(%s)0x%" PRIx64, CSTR(tname), addr);
            JSON_NEW_STRING_FIELD(item, "value", tmp);
            value_added = 1;
            if (!pointer_size) {
                dim = 1;
            }
            /* FALLTHROUGH - process pointer as array */
        case TKIND_ARRAY:
            if (!indirect) {
                /* check parent base type */
                if (    DAB_OK != DAB_CURSOR_RESET(type_cursor) ||
                        DAB_OK != DAB_CURSOR_BIND(type_cursor, base_type) ||
                        DAB_OK != DAB_CURSOR_FETCH(type_cursor, &size, &dummy, &flags, &base_type)) {
                    RETCLEAN(FAILURE);
                }
            }
            if (pointer_size) {
                dim = pointer_size / size;      // number of base type elements in allocated pointer
            }

            /* normally it must be in pointer processing but I moved it here to be after query to base type */
            if (TKIND_FUNC == (flags & TKIND_TYPE)) {
                /* process function pointer - find function name */
                char *fun_name;
                char *value;
                if (SUCCESS != func_name(addr, &fun_name)) {
                    value = malloc(32);
                    sprintf(value, "0x%" PRIx64 " (invalid)", addr);
                } else {
                    value = malloc(strlen(fun_name) + 32);
                    sprintf(value, "0x%" PRIx64 " <%s>", addr, fun_name);
                    free(fun_name);
                }
                JSON_NEW_STRING_FIELD(item, "value", value);
                JSON_NEW_INT64_FIELD(item, "variablesReference", 0);
                free(value);
                RETCLEAN(SUCCESS);
            }

            if (size == 1 && (TKIND_SIGNED == (flags & TKIND_TYPE) || TKIND_UNSIGNED == (flags & TKIND_TYPE))) {
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
                JSON_NEW_STRING_FIELD(item, "value", value);
                JSON_NEW_INT64_FIELD(item, "variablesReference", 0);
                free(value);
                RETCLEAN(SUCCESS);
            }

            if (SUCCESS != get_var_ref(parent_type, parent, name, addr, type, indirect, &ref)) {
                RETCLEAN(FAILURE);
            }
            if (!value_added) {      // add value with type, if not added by pointer code above
                JSON_NEW_STRING_FIELD(item, "value", CSTR(tname));
            }
            JSON_NEW_INT64_FIELD(item, "variablesReference", ref);
            JSON_NEW_INT64_FIELD(item, "indexedVariables", dim);
            RETCLEAN(SUCCESS);
        case TKIND_SIGNED:
            mem = get_var_value(addr, size, cur_step);
            if (!mem) {
                RETCLEAN(FAILURE);
            }
            switch (size) {
                case 1:
                    if (isprint(*(int8_t *)mem)) {
                        sprintf(new_val, "%" PRId8 " '%c'", *(int8_t *)mem, *(int8_t *)mem);
                    } else {
                        sprintf(new_val, "%" PRId8, *(int8_t *)mem);
                    }
                    break;
                case 2:
                    sprintf(new_val, "%" PRId16, *(int16_t *)mem);
                    break;
                case 4:
                    sprintf(new_val, "%" PRId32, *(int32_t *)mem);
                    break;
                case 8:
                    sprintf(new_val, "%" PRId64, *(int64_t *)mem);
                    break;
                default:
                    ERR("Unsupported %" PRId64 "-byte long integer var %s", size, name);
                    strcpy(new_val, "unsupported");
                    break;
            }
            break;
        case TKIND_UNSIGNED:
            mem = get_var_value(addr, size, cur_step);
            if (!mem) {
                RETCLEAN(FAILURE);
            }
            switch (size) {
                case 1:
                    if (isprint(*(uint8_t *)mem)) {
                        sprintf(new_val, "%" PRIu8 " '%c'", *(uint8_t *)mem, *(uint8_t *)mem);
                    } else {
                        sprintf(new_val, "%" PRIu8, *(uint8_t *)mem);
                    }
                    break;
                case 2:
                    sprintf(new_val, "%" PRIu16, *(uint16_t *)mem);
                    break;
                case 4:
                    sprintf(new_val, "%" PRIu32, *(uint32_t *)mem);
                    break;
                case 8:
                    sprintf(new_val, "%" PRIu64, *(uint64_t *)mem);
                    break;
                default:
                    ERR("Unsupported %" PRId64 "-byte long integer var %s", size, name);
                    strcpy(new_val, "unsupported");
                    break;
            }
            break;
        case TKIND_FLOAT:
            mem = get_var_value(addr, size, cur_step);
            if (!mem) {
                RETCLEAN(FAILURE);
            }
            switch (size) {
                case __SIZEOF_FLOAT__:
                    sprintf(new_val, "%g", *(float *)mem);
                    break;
                case __SIZEOF_DOUBLE__:
                    sprintf(new_val, "%lg", *(double *)mem);
                    break;
#if __SIZEOF_LONG_DOUBLE__ > __SIZEOF_DOUBLE__      // to avoid compile error with equal cases
                case __SIZEOF_LONG_DOUBLE__:
                    sprintf(new_val, "%Lg", *(long double *)mem);
                    break;
#endif
                default:
                    ERR("Unsupported %" PRId64 "-byte long float var %s", size, name);
                    strcpy(new_val, "unsupported");
                    break;
            }
            break;
        case TKIND_ENUM:
            mem = get_var_value(addr, size, cur_step);
            if (!mem) {
                RETCLEAN(FAILURE);
            }
            uint32_t value = 0;
            new_val[0] = '\0';
            switch (size) {                
                case 1:
                    value = *(uint8_t *)mem;
                    break;
                case 2:
                    value = *(uint16_t *)mem;
                    break;
                case 4:
                    value = *(uint32_t *)mem;
                    break;
                default:
                    ERR("Unsupported %" PRId64 "-byte long enum %s", size, name);
                    strcpy(new_val, "unsupported");
                    break;
            }
            if (new_val[0]) {
                break;
            }
            /* lookup enum item name by value */
            if (!enum_cursor) {
                if (DAB_OK != DAB_CURSOR_OPEN(&enum_cursor,
                    "SELECT "
                        "name "
                    "FROM "
                        "member "
                    "WHERE "
                        "offset = ? AND "
                        "value = ?",
                        type, value
                )) {
                    free(mem);
                    RETCLEAN(FAILURE);
                }
            } else if (DAB_OK != DAB_CURSOR_RESET(enum_cursor) || DAB_OK != DAB_CURSOR_BIND(enum_cursor, type, value)) {
                free(mem);
                RETCLEAN(FAILURE);
            }
            char *item_name;
            if (DAB_OK != DAB_CURSOR_FETCH(enum_cursor, &item_name)) {
                sprintf(new_val, "%" PRIu32, value);
            } else {
                sprintf(new_val, "%s (%" PRIu32 ")", item_name, value);
            }          
            break;
        default:
            ERR("Type %" PRIu64 " not implemented yet", flags & TKIND_TYPE);
            strcpy(new_val, "unsupported");
            break;
    }
    JSON_NEW_INT64_FIELD(item, "variablesReference", 0);
    JSON_NEW_STRING_FIELD(item, "value", new_val);

    free(mem);

cleanup:
    STRFREE(tname);

    return ret;
}


/**************************************************************************
 *
 *  Function:   get_var_ref
 *
 *  Params:     parent_type - type of parent reference
 *              parent - id of parent reference
 *              child - name of child (var / var element)
 *              address
 *              type - variable type
 *              indirect - number of indirections for the type
 *              ref - where to write the reference
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Add variable to reference table, return the reference
 *
 **************************************************************************/
int get_var_ref(int parent_type, ULONG parent, const char *child, ULONG address, ULONG type, int indirect, ULONG *ref) {
    if (!ref_cursor) {
        /* create in-memory table for var references */
        if (DAB_OK != DAB_EXEC("CREATE TABLE local.ref ("
                "id             INTEGER PRIMARY KEY AUTOINCREMENT, "
                "parent_type    INTEGER, "
                "parent         INTEGER, "
                "child          VARCHAR, "
                "address        INTEGER, "
                "type           INTEGER, "
                "indirect       INTEGER DEFAULT 0, "
                "UNIQUE (parent_type, parent, child)"
            ")"
        )) {
            return FAILURE;
        }
        /* unlike 'INSERT OR REPLACE', 'ON CONFLICT DO UPDATE' doesn't delete the existing record so 'id' remains */
        if (DAB_OK != DAB_CURSOR_PREPARE(&ref_upsert, "INSERT INTO local.ref "
                "(parent_type, parent, child, address, type, indirect) "
                "VALUES "
                "(?,           ?,      ?,     ?,       ?,    ?) "
                "ON CONFLICT (parent_type, parent, child) "
                    "DO UPDATE SET "
                        "address = excluded.address, "
                        "type = excluded.type, "
                        "indirect = excluded.indirect"
        )) {
            return FAILURE;
        }
        /* cannot use last inserted ID in case of UPSERT */
        if (DAB_OK != DAB_CURSOR_PREPARE(&ref_cursor, "SELECT id FROM local.ref "
                "WHERE "
                    "parent_type = ? AND "
                    "parent = ? AND "
                    "child = ?"
        )) {
            return FAILURE;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(ref_upsert) ||
                DAB_OK != DAB_CURSOR_RESET(ref_cursor)) {
        return FAILURE;
    }

    if (    DAB_OK != DAB_CURSOR_BIND(ref_upsert, parent_type, parent, child, address, type, indirect) ||
            DAB_NO_DATA != DAB_CURSOR_FETCH(ref_upsert) ||
            DAB_OK != DAB_CURSOR_BIND(ref_cursor, parent_type, parent, child) ||
            DAB_OK != DAB_CURSOR_FETCH(ref_cursor, ref)) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   get_pointer_size
 *
 *  Params:     address
 *              size - where to store size of allocated memory
 *
 *  Return:     SUCCESS / FAILURE / NOTFOUND
 *
 *  Descr:      Look for pointer content regardless of its location
 *
 **************************************************************************/
int get_pointer_size(ULONG address, ULONG *size) {
    /* try the heap first */
    if (!heap_cursor) {
        /* get the most recent allocation for this variable */
        if (DAB_OK != DAB_CURSOR_OPEN(&heap_cursor,
            "SELECT "
                "size, "
                "freed_at "
            "FROM "
                "heap "
            "WHERE "
                "address = ? AND "
                "allocated_at <= ? "
            "ORDER BY "
                "allocated_at DESC", address, cur_step
        )) {
        return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(heap_cursor) || DAB_OK != DAB_CURSOR_BIND(heap_cursor, address, cur_step)) {
        return FAILURE;
    }

    ULONG freed_at;
    int ret = DAB_CURSOR_FETCH(heap_cursor, size, &freed_at);
    if (DAB_OK == ret) {        // found address on heap
        if (freed_at && freed_at <= cur_step) {
            return MEM_RELEASED;    // memory was released, address doesn't point to allocated memory
        }
        return SUCCESS;
    } else {
        /* TODO: check if memory really belongs to the process - require storing memory map in DB */
        /* TODO: find the real variable the address belongs to, and find real size */
        *size = 0;
        return SUCCESS;
    }

    return MEM_NOTFOUND;
}


/**************************************************************************
 *
 *  Function:   func_name
 *
 *  Params:     address
 *              name - where to store function name
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Lookup function name by address
 *
 **************************************************************************/
int func_name(ULONG address, char **name) {
    address -= program_base_addr;
    if (!func_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&func_cursor,
            "SELECT "
                "f.name "
            "FROM "
                "statement s "
                "JOIN function f ON "
                    "f.id = s.function_id "
            "WHERE "
                "s.address = ?", address
        )) {
        return FAILURE;
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(func_cursor) || DAB_OK != DAB_CURSOR_BIND(func_cursor, address)) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_FETCH(func_cursor, name)) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   type_name
 *
 *  Params:     type_offset
 *              type_name - where to store function name
 *              indirect - number of indirections to add
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Lookup type name by offset
 *
 **************************************************************************/
struct sr *type_name(ULONG type_offset, int indirect) {
    if (!type_name_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&type_name_cursor,
            "WITH RECURSIVE parent_of(name, flags, offset, parent, level) AS ("
                "SELECT "
                    "name, "
                    "flags, "
                    "offset, "
                    "parent, "
                    "0 "
                "FROM "
                    "type "
                "WHERE "
                    "offset = ? "
                "UNION "
                "SELECT "
                    "type.name, "
                    "type.flags, "
                    "type.offset, "
                    "type.parent, "
                    "parent_of.level + 1 "
                "FROM "
                    "type, "
                    "parent_of "
                "WHERE "
                    "type.offset = parent_of.parent "
                ") "
            " SELECT "
                "name, "
                "flags "
            "FROM "
                "parent_of "
            "ORDER BY "
                "level DESC",
            type_offset
        )) {
            return NULL;
        }
    } else if ( DAB_OK != DAB_CURSOR_RESET(type_name_cursor) ||
                DAB_OK != DAB_CURSOR_BIND(type_name_cursor, type_offset)) {
        return NULL;
    }

    char *name;
    ULONG flags;
    struct sr *res = sr_new("", 32);
    while (DAB_OK == DAB_CURSOR_FETCH(type_name_cursor, &name, &flags)) {
        switch (flags & TKIND_TYPE) {
            case TKIND_POINTER:
                STRCAT(res, "*");
                break;
            case TKIND_STRUCT:
                CONCAT(res, "struct ", name);
                break;
            case TKIND_UNION:
                CONCAT(res, "union ", name);
                break;
            case TKIND_ARRAY:
                STRCAT(res, "[]");
                break;
            case TKIND_SIGNED:
            case TKIND_UNSIGNED:
            case TKIND_FLOAT:
                STRCAT(res, name);
                break;
        }
        free(name);
    }
    for (; indirect > 0; indirect--) {
        STRCAT(res, "*");
    }

    return res;
}
