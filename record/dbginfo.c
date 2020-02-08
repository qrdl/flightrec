/**************************************************************************
 *
 *  File:       dbg_info.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Collect DWARF debug info from executable
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
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <libgen.h>
#include <libdwarf/libdwarf.h>
#include <libdwarf/dwarf.h>

#include "stingray.h"
#include "dab.h"
#include "eel.h"

#include "flightrec.h"
#include "record.h"

/* attribute flags to control attribute presense and cleanup requirements */
#define AF_PRESENT 0x01
#define AF_CLEANUP 0x02

#define ATTR_LIST(...)      struct die_attr attr_list[] = { __VA_ARGS__, {0}}
#define ATTR(A,B)           {A, B, 0, 0}
#define ATTR_PRESENT(...)   attr_present(attr_list, __VA_ARGS__, -1)

/* macro for cursor execution */
#define CURSOR_EXEC(C, ...) do { \
                                if (DAB_OK != DAB_CURSOR_RESET(C)) RETCLEAN(FAILURE); \
                                if (DAB_OK != DAB_CURSOR_BIND(C, __VA_ARGS__)) RETCLEAN(FAILURE); \
                                if (DAB_FAIL == DAB_CURSOR_FETCH(C)) RETCLEAN(FAILURE); \
                            } while (0)

struct die_attr {
    long    attr_id;
    void    *var;
    char    flags;
    int     cleanup_flag;            // if non-zero - cleanup is required
};

static int proc_unit(Dwarf_Debug dbg);
static int proc_lines(Dwarf_Debug dbg, Dwarf_Die cu_die, ULONG unit_id);
static int proc_symbols(Dwarf_Debug dbg, Dwarf_Die parent_die, ULONG unit_id, ULONG scope_id, ULONG depth);
static int proc_func(Dwarf_Debug dbg, Dwarf_Die die, ULONG unit_id, ULONG scope_id, ULONG depth);
static int proc_block(Dwarf_Debug dbg, Dwarf_Die die, ULONG unit_id, ULONG scope_id, ULONG depth);
/* processing type info */
static int proc_base_type(Dwarf_Debug dbg, Dwarf_Die die, ULONG unit_id);
static int proc_custom_type(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half tag, ULONG unit_id);
static int proc_array_type(Dwarf_Debug dbg, Dwarf_Die parent_die, ULONG unit_id);
static int proc_aggr_type(Dwarf_Debug dbg, Dwarf_Die parent_die, Dwarf_Half tag, ULONG unit_id);
static int proc_aggr_member(Dwarf_Debug dbg, Dwarf_Die parent_die, ULONG unit_id, Dwarf_Off offset);
static int proc_var(Dwarf_Debug dbg, Dwarf_Die die, ULONG scope_id, ULONG unit_id);


static int get_attrs(Dwarf_Debug dbg, Dwarf_Die die, struct die_attr*attr_list);
static void cleanup_attrs(Dwarf_Debug dbg, struct die_attr *attr_list);
static int attr_present(struct die_attr *attr_list, ...);
static Dwarf_Off die_offset(Dwarf_Die die);

static Dwarf_Signed    cnt_file;
static char            *unitdir = NULL;
static ULONG            *fileids;   // array of source files - mapping between DWARF file name and file ID in DB
static Dwarf_Addr      cu_base_address;       // unit base address - some addresses are expressed as offset from base


/**************************************************************************
 *
 *  Function:   dbg_srcinfo
 *
 *  Params:     name - name (incl. path) of the program to process
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Entry point for collecting debug information - the only
 *              public function in this unit
 *
 **************************************************************************/
int dbg_srcinfo(char *name) {
    int     fd = -1;
    int ret = SUCCESS;
    Dwarf_Error err = NULL;
    Dwarf_Debug dbg = NULL;

    if (SUCCESS != create_db()) {
        ERR("Cannot create DB structure");
        return FAILURE;
    }

    fd = open(name, O_RDONLY);
    if (!fd) {
        ERR("Cannot open %s - %s", name, strerror(errno));
        RETCLEAN(FAILURE);
    }

    if (DW_DLV_ERROR == dwarf_init(fd, DW_DLC_READ, NULL, NULL, &dbg, &err)) {
        ERR("DWARF init failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    } else if (DW_DLV_NO_ENTRY == ret) {
        ERR("No DWARF information found");
        RETCLEAN(FAILURE);
    }

    if (SUCCESS != prepare_statements()) {
        RETCLEAN(FAILURE);
    }

    while (SUCCESS == (ret = proc_unit(dbg)));
    if (END == ret) {
        ret = SUCCESS;   // all units processed ok
    }

    if (SUCCESS != alter_db()) {
        ERR("Cannot alter DB structure");
        return FAILURE;
    }
 
cleanup:
    if (fd > 0) {
        close(fd);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }
    if (dbg) {
        dwarf_finish(dbg, &err);
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
        /* dbg cannot be freed, according to libdwarf manual rev 1.63, Sep'06 */
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_unit
 *
 *  Params:     dbg - debug handle
 *
 *  Return:     SUCCESS / END / FAILURE
 *
 *  Descr:      Process next compilation unit
 *
 **************************************************************************/
int proc_unit(Dwarf_Debug dbg) {
    Dwarf_Unsigned  header_length = 0, abbrev_offset = 0, next_offset = 0;
    Dwarf_Half      version_stamp = 0, address_size = 0;
    Dwarf_Die       cu_die = NULL;
    char            *path = NULL;
    char            *name = NULL;
    ATTR_LIST(
        ATTR(DW_AT_comp_dir,    &path),
        ATTR(DW_AT_name,        &name),
        ATTR(DW_AT_low_pc,      &cu_base_address)
    );
    Dwarf_Error err = NULL;
    int ret = SUCCESS;
    int in_txn = 0;

    ret = dwarf_next_cu_header(dbg, &header_length, &version_stamp, &abbrev_offset, &address_size, &next_offset, &err);
    if (DW_DLV_ERROR == ret) {
        ERR("Getting unit header failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    } else if (DW_DLV_NO_ENTRY == ret) {
        RETCLEAN(END);
    }

    if (DW_DLV_ERROR == dwarf_siblingof(dbg, NULL /* NULL means first entry */, &cu_die, &err)) {
        ERR("Getting sibling DIE failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    } else if (DW_DLV_NO_ENTRY == ret) {
        ERR("No sibling DIE found");
        RETCLEAN(FAILURE);
    }

    ret = get_attrs(dbg, cu_die, attr_list);
    if (SUCCESS != ret) {
        RETCLEAN(ret);
    }

    /* check if source is from allowed path */
    if (ATTR_PRESENT(0) && path && strncmp(path, acceptable_path, strlen(acceptable_path))) {
        RETCLEAN(SUCCESS);
    }

    /* trim leading path to allow further relocation of sources */
    path += strlen(acceptable_path);
    if ('/' == *path) {
        path++;     // don't want leading slash in the path
    }
    if (ATTR_PRESENT(1) && name) {
        name = basename(name);
        struct entry *tmp;
        if (process_unit) {
            for (tmp = process_unit; tmp; tmp = tmp->next) {
                if (!strcmp(tmp->name, name)) {
                    break;      // found white-listed unit
                }
            }
            if (!tmp) {
                RETCLEAN(SUCCESS);      // unit is not white-listed
            }
        } else if (ignore_unit) {
            for (tmp = ignore_unit; tmp; tmp = tmp->next) {
                if (!strcmp(tmp->name, name)) {
                    RETCLEAN(SUCCESS);      // found black-listed unit
                }
            }
        }
    }

    INFO("Processing unit %s", name);
    if (DAB_OK != DAB_EXEC("BEGIN")) {
        RETCLEAN(FAILURE);
    } else {
        in_txn = 1;
    }

    if (DAB_OK != DAB_EXEC("INSERT "
                                "INTO unit "
                                "(name, path, base_addr) VALUES "
                                "(?,    ?,    ?)",
                                  name, path, cu_base_address)) {
        RETCLEAN(FAILURE);
    }
    long unit_id = DAB_LAST_ID;
    if (unitdir) {
        free(unitdir);
    }
    unitdir = strdup(path);
    /* collect file and file line info */
    ret = proc_lines(dbg, cu_die, unit_id);
    if (SUCCESS != ret) {
        RETCLEAN(ret);
    }

    ret = proc_symbols(dbg, cu_die, unit_id, GLOBAL_SCOPE, 0);   // zero depth
    if (SUCCESS != ret) {
        RETCLEAN(ret);
    }

    if (DAB_OK != DAB_EXEC("COMMIT")) {
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    if (SUCCESS != ret && in_txn) {
        DAB_EXEC("ROLLBACK");
    }
    cleanup_attrs(dbg, attr_list);
    if (cu_die) {
        dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_lines
 *
 *  Params:     dbg - debug handle
 *              cu_die - debug info entry for compilation unit
 *              unit_id - unit ID
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process .debug_lines section
 *
 **************************************************************************/
int proc_lines(Dwarf_Debug dbg, Dwarf_Die cu_die, ULONG unit_id) {
    int             i;
    Dwarf_Line      *filelines = NULL;
    char            **filenames = NULL;
    Dwarf_Signed    cnt_line = 0;
    Dwarf_Addr      address = 0;
    Dwarf_Unsigned  lineno = 0, fileno = 0;
    sr_string       abspath = NULL;
    Dwarf_Error     err = NULL;
    int             ret = SUCCESS;

    /***** process all files for the unit, store only relevant ones */
    if (DW_DLV_ERROR == dwarf_srcfiles(cu_die, &filenames, &cnt_file, &err)) {
        ERR("Getting source file names failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    if (fileids) {
        free(fileids);
    }
    fileids = malloc(sizeof(*fileids) * cnt_file);

    for (i = 0; i < cnt_file; i++) {
        abspath = get_abs_path(unitdir, filenames[i]);
        if (!strncmp(abspath->val, acceptable_path, strlen(acceptable_path))) {
            /* trim leading path to allow further relocation of sources */
            char *path = abspath->val + strlen(acceptable_path);
            if ('/' == *path) {
                path++;     // don't want leading slash in the path
            }

            char *tmp = strdup(filenames[i]);
            ret = DAB_EXEC("INSERT "
                                "INTO file "
                                "(name,          path, unit_id, seq) VALUES "
                                "(?,             ?,    ?,       ?)",
                                  basename(tmp), path, unit_id, i+1);    // DWARF indexes files from 1
            free(tmp);
            if (DAB_OK != ret) {
                RETCLEAN(FAILURE);
            }
            fileids[i] = DAB_LAST_ID;
            DBG("File %d: %s, fid %ld", i + 1, filenames[i], fileids[i]);
        } else {
            fileids[i] = 0;    // file not indexed
        }
        STRFREE(abspath);
    }

    ret = SUCCESS;
    /***** process line info for the unit, log only lines for relevant files *****/
    if (DW_DLV_ERROR == dwarf_srclines(cu_die, &filelines, &cnt_line, &err)) {
        ERR("Getting source file lines failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    for (i = 0; i < cnt_line; i++) {
        /* process only lines that are beginning of the statement and not end-of-text */
        Dwarf_Bool is;
        if (DW_DLV_ERROR == dwarf_linebeginstatement(filelines[i], &is, &err)) {
            ERR("Getting line begin attribute failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }
        if (!is) {
            continue;       // skip not begin-of-statement addresses
        }
        if (DW_DLV_ERROR == dwarf_lineendsequence(filelines[i], &is, &err)) {
            ERR("Getting line end statement attribute failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }
        if (is) {
            continue;       // skip end-of-unit
        }
        if (DW_DLV_ERROR == dwarf_line_srcfileno(filelines[i], &fileno, &err)) {
            ERR("Getting line file failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }
        if (!fileids[fileno - 1]) {
            continue;       // file is outside allowed path - skip
        }
        if (DW_DLV_ERROR == dwarf_lineaddr(filelines[i], &address, &err)) {
            ERR("Getting line address failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }
        if (DW_DLV_ERROR == dwarf_lineno(filelines[i], &lineno, &err)) {
            ERR("Getting line number failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }

        CURSOR_EXEC(insert_line, fileids[fileno - 1], lineno, address);
    }

cleanup:
    if (filenames) {
        for (i = 0; i < cnt_file; i++) {
            dwarf_dealloc(dbg, filenames[i], DW_DLA_STRING);
        }
        dwarf_dealloc(dbg, filenames, DW_DLA_LIST);
    }
    if (filelines) {
        dwarf_srclines_dealloc(dbg, filelines, cnt_line);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_symbols
 *
 *  Params:     dbg - debug handle
 *              parent_die - debug info entry for compilation unit, function or lexical block
 *              unit_id - unit ID
 *              scope_id - ID of scope / NO_SCOPE (0)
 *              depth - scope depth
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process variables, functions, types etc.
 *
 **************************************************************************/
int proc_symbols(Dwarf_Debug dbg, Dwarf_Die parent_die, ULONG unit_id, ULONG scope_id, ULONG depth) {
    Dwarf_Half  tag = 0;
    Dwarf_Die   die = NULL, old_die = NULL;
    Dwarf_Error err = NULL;
    int ret = SUCCESS;

    for (ret = dwarf_child(parent_die, &die, &err); DW_DLV_OK == ret; ret = dwarf_siblingof(dbg, old_die, &die, &err)) {
        // old_die is needed to properly release memory
        if (old_die) {
            dwarf_dealloc(dbg, old_die, DW_DLA_DIE);
        }
        old_die = die;

        if (DW_DLV_ERROR == dwarf_tag(die, &tag, &err)) {
            ERR("Getting DIE's tag failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }

        switch (tag) {
            case DW_TAG_compile_unit:
                /* FIXME: Not sure it is possible to get this type as child of unit DIE */
                break;
            case DW_TAG_variable:   /* FALLTHROUGH */
            case DW_TAG_formal_parameter:
                if (SUCCESS != proc_var(dbg, die, scope_id, unit_id)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_subprogram:
                if (SUCCESS != proc_func(dbg, die, unit_id, scope_id, depth)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_structure_type: /* FALLTHROUGH */
            case DW_TAG_union_type:     /* FALLTHROUGH */
            case DW_TAG_enumeration_type:
                /* collect aggregate type */
                if (SUCCESS != proc_aggr_type(dbg, die, tag, unit_id)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_lexical_block:
                if (SUCCESS != proc_block(dbg, die, unit_id, scope_id, depth)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_base_type:
                /* Process simple type */
                if (SUCCESS != proc_base_type(dbg, die, unit_id)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_array_type:
                /* Process simple type */
                if (SUCCESS != proc_array_type(dbg, die, unit_id)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_typedef:            /* FALLTHROUGH */
            case DW_TAG_pointer_type:       /* FALLTHROUGH */
            case DW_TAG_const_type:         /* FALLTHROUGH */
            case DW_TAG_volatile_type:      /* FALLTHROUGH */
            case DW_TAG_subroutine_type:    /* FALLTHROUGH */
            case DW_TAG_restrict_type:
                /* Process derived type type */
                if (SUCCESS != proc_custom_type(dbg, die, tag, unit_id)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_label:                  /* FALLTHROUGH */
            case DW_TAG_unspecified_parameters: /* FALLTHROUGH */
            case DW_TAG_unspecified_type:       /* FALLTHROUGH */
            case DW_TAG_namespace:
                /* ignore */
                break;
            default:
                ERR("Unknown tag 0x%x at offset %llx", tag, die_offset(die));
                RETCLEAN(FAILURE);
        }
    }

    if (DW_DLV_NO_ENTRY == ret) {
        RETCLEAN(SUCCESS);   // reached end of chain
    } else {
        ERR("Getting DIE failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

cleanup:
    if (die) {
        dwarf_dealloc(dbg, die, DW_DLA_DIE);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_func
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for functions and function types
 *              scope_id - ID of scope / NO_SCOPE (0)
 *              depth - scope depth
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process function and TODO: function type
 *
 **************************************************************************/
int proc_func(Dwarf_Debug dbg, Dwarf_Die die, ULONG unit_id, ULONG scope_id, ULONG depth) {
    char        *name = NULL;
    int         external = 0;
    Dwarf_Half  tag = 0;
    Dwarf_Addr  lo_addr = 0, hi_addr = 0;
    ATTR_LIST(
        ATTR(DW_AT_name,        &name),
        ATTR(DW_AT_low_pc,      &lo_addr),
        ATTR(DW_AT_high_pc,     &hi_addr),
        ATTR(DW_AT_external,    &external)
    );
    Dwarf_Error err = NULL;
    int ret = SUCCESS;

    if (SUCCESS != get_attrs(dbg, die, attr_list)) {
        RETCLEAN(FAILURE);
    }
    if (DW_DLV_ERROR == dwarf_tag(die, &tag, &err)) {
        ERR("Getting DIE's tag failed - %s (offset %llx)", dwarf_errmsg(err), die_offset(die));
        RETCLEAN(FAILURE);
    }

    Dwarf_Off offset = die_offset(die);

    if (DW_TAG_subprogram == tag) {
        if (!ATTR_PRESENT(0)) {
            ERR("Missing function name (offset %llx)", offset);
            RETCLEAN(FAILURE);
        }
        if (!ATTR_PRESENT(1, 2)) {
            if (ATTR_PRESENT(3) && external)
                RETCLEAN(SUCCESS);       // ignore forward declaration
            else {
                ERR("Missing function %s address(es) (offset %llx)", name, offset);
                RETCLEAN(FAILURE);
            }
        }

        /* sometimes in DWARF4 high address is just offset from low */
        if (hi_addr < lo_addr) {
            hi_addr += lo_addr;
        }

//        DBG("Processing function %s (depth %lu) from %llx to %llx", name, depth, lo_addr, hi_addr);
        CURSOR_EXEC(insert_scope, scope_id, depth, lo_addr, hi_addr);
        scope_id = DAB_LAST_ID;
        CURSOR_EXEC(insert_func, name, scope_id, offset);

        if (SUCCESS != proc_symbols(dbg, die, unit_id, scope_id, depth + 1)) {
            RETCLEAN(FAILURE);
        }
    } else {
        /* TODO: Process function type */
    }

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_block
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for block
 *              unit_id
 *              scope_id - ID of scope / NO_SCOPE (0)
 *              depth - scope depth
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process lexical block
 *
 *  Comment:    DWARF contains info only about blocks with symbols
 *              declared inside
 *
 **************************************************************************/
int proc_block(Dwarf_Debug dbg, Dwarf_Die die, ULONG unit_id, ULONG scope_id, ULONG depth) {
    Dwarf_Addr  lo_addr = 0, hi_addr = 0;
    Dwarf_Off   ranges_off = 0;
    ATTR_LIST(
        ATTR(DW_AT_low_pc,  &lo_addr),
        ATTR(DW_AT_high_pc, &hi_addr),
        ATTR(DW_AT_ranges,  &ranges_off)
    );
    Dwarf_Error err = NULL;
    int ret = SUCCESS;

    if (SUCCESS != get_attrs(dbg, die, attr_list)) {
        RETCLEAN(FAILURE);
    }

    if (!ATTR_PRESENT(0, 1) && ATTR_PRESENT(2)) {
        /* block defined as ranges */
        Dwarf_Ranges *ranges;
        Dwarf_Signed count;
        ret = dwarf_get_ranges(dbg, ranges_off, &ranges, &count, NULL, &err);
        if (DW_DLV_OK != ret) {
            ERR("Getting ranges failed - %s (offset %llx)", dwarf_errmsg(err), die_offset(die));
            RETCLEAN(FAILURE);
        }

        /* consider ranges as contiguous block, take start line from first one, and finish line from last one */
        lo_addr = cu_base_address + ranges[0].dwr_addr1;
        hi_addr = cu_base_address + ranges[count-2].dwr_addr2;      // Last range entry is terminator, so take the penultimate one

        dwarf_ranges_dealloc(dbg, ranges, count);
    }

    /* sometimes in DWARF4 high address is just offset from low */
    if (hi_addr < lo_addr) {
        hi_addr += lo_addr;
    }

//    DBG("Processing lexical block (depth %lu)", depth);
    CURSOR_EXEC(insert_scope, scope_id, depth, lo_addr, hi_addr);
    scope_id = DAB_LAST_ID;
    if (SUCCESS != proc_symbols(dbg, die, unit_id, scope_id, depth + 1)) {
        RETCLEAN(FAILURE);
    }

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_base_type
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for type
 *              unit_id - unit ID
 *
 *  Return:     SUCCESS / FAIL
 *
 *  Descr:      Process base type
 *
 **************************************************************************/
int proc_base_type(Dwarf_Debug dbg, Dwarf_Die die, ULONG unit_id) {
    char            *name = NULL;
    Dwarf_Unsigned  size = 0;
    Dwarf_Unsigned  encoding = 0;
    Dwarf_Off       offset;
    Dwarf_Error     err = NULL;
    int             ret = SUCCESS;
    ATTR_LIST(
        ATTR(DW_AT_name,        &name),
        ATTR(DW_AT_byte_size,   &size),
        ATTR(DW_AT_encoding,    &encoding)
    );

    if (DW_DLV_ERROR == dwarf_dieoffset(die, &offset, &err)) {
        ERR("Getting DIE's offset failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    if (SUCCESS != get_attrs(dbg, die, attr_list)) {
        RETCLEAN(FAILURE);
    }
    if (!ATTR_PRESENT(0, 1, 2)) {
        WARN("Basic type is missing mandatory attributes (offset 0x%llx)", offset);
        RETCLEAN(FAILURE);
    }

    int tkind = 0;
    switch (encoding) {
        case DW_ATE_signed:     /* FALLTHROUGH */
        case DW_ATE_signed_char:
            tkind = TKIND_SIGNED;
            break;
        case DW_ATE_unsigned:   /* FALLTHROUGH */
        case DW_ATE_unsigned_char:
            tkind = TKIND_UNSIGNED;
            break;
        case DW_ATE_float:
            tkind = TKIND_FLOAT;
            break;
        default:
            ERR("Unknown encoding %llx for type", encoding);
    }

    CURSOR_EXEC(insert_type, name, size, tkind, unit_id, (ULLONG)offset, 0);

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_custom_type
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for type
 *              tag - DWARF tag for type
 *              unit_id - unit ID
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process custom (non-base) type (except struct, union and enums)
 *
 **************************************************************************/
int proc_custom_type(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half tag, ULONG unit_id) {
    char            *name = NULL;
    Dwarf_Off       offset;
    size_t          size = 0;
    Dwarf_Error     err = NULL;
    int             ret = SUCCESS;
    int             kind;
    ULONG           typeref = 0;
    ATTR_LIST(
        ATTR(DW_AT_type,        &typeref),  // reference to underlying type
        ATTR(DW_AT_name,        &name),
        ATTR(DW_AT_byte_size,   &size)      // pointer needs size
    );

    if (DW_DLV_ERROR == dwarf_dieoffset(die, &offset, &err)) {
        ERR("Getting DIE's offset failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    if (SUCCESS != get_attrs(dbg, die, attr_list)) {
        RETCLEAN(FAILURE);
    }

    if (!name) {
        name = "";      // anonymous type
    }
    if (!ATTR_PRESENT(0)) {
        if (DW_TAG_pointer_type == tag) {
            name = "void";
        } else {
            /* no base type specified - either forward type declaration or opaque type,
               no variable of this type can exist, only the pointer */
            RETCLEAN(SUCCESS);
        }
    }

    switch (tag) {
        case DW_TAG_typedef:
            kind = TKIND_ALIAS;
            break;
        case DW_TAG_array_type:
            kind = TKIND_ARRAY;
            // TODO: process DW_TAG_subrange_type with DW_AT_upper_bound to get array size
            break;
        case DW_TAG_pointer_type:
            kind = TKIND_POINTER;
            break;
        case DW_TAG_subroutine_type:
            kind = TKIND_FUNC;
            break;
        case DW_TAG_const_type:
            kind = TKIND_CONST;
            break;
        case DW_TAG_restrict_type:
            kind = TKIND_RESTRICT;
            break;
        case DW_TAG_volatile_type:
            kind = TKIND_VOLATILE;
            break;
        default:
            ERR("Unsupported tag 0x%x for derived type (offset 0x%llx)", tag, offset);
            RETCLEAN(FAILURE);
    }

    CURSOR_EXEC(insert_type, name, size, kind, unit_id, (ULLONG)offset, typeref);

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_array_type
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for type
 *              unit_id - unit ID
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process array type
 *
 **************************************************************************/
int proc_array_type(Dwarf_Debug dbg, Dwarf_Die parent_die, ULONG unit_id) {
    Dwarf_Die       die = NULL, old_die = NULL;
    Dwarf_Off       offset;
    ULONG           basetype;
    Dwarf_Error     err = NULL;
    Dwarf_Half      tag;
    Dwarf_Unsigned  array_size = 0;
    int             ret = SUCCESS;
    ATTR_LIST(
        ATTR(DW_AT_type,        &basetype)
    );

    if (DW_DLV_ERROR == dwarf_dieoffset(parent_die, &offset, &err)) {
        ERR("Getting DIE's offset failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    if (SUCCESS != get_attrs(dbg, parent_die, attr_list)) {
        RETCLEAN(FAILURE);
    }

    if (!ATTR_PRESENT(0)) {
        ERR("Missing base type for array at offset 0x%llx)", offset);
        RETCLEAN(FAILURE);
    }

    /* get array size from DW_TAG_subrange_type -> DW_AT_upper_bound */
    for (ret = dwarf_child(parent_die, &die, &err); DW_DLV_OK == ret; ret = dwarf_siblingof(dbg, old_die, &die, &err)) {
        if (old_die) {
            dwarf_dealloc(dbg, old_die, DW_DLA_DIE);
        }
        old_die = die;
        if (DW_DLV_ERROR == dwarf_tag(die, &tag, &err)) {
            ERR("Getting DIE's tag failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }
        if (DW_TAG_subrange_type == tag) {
            Dwarf_Attribute attrib = NULL;
            ret = dwarf_attr(die, DW_AT_upper_bound, &attrib, &err);
            if (DW_DLV_ERROR == ret) {
                ERR("Getting attribute failed - %s", dwarf_errmsg(err));
                RETCLEAN(FAILURE);
            } else if (DW_DLV_NO_ENTRY != ret) {
                if (DW_DLV_ERROR == dwarf_formudata(attrib, &array_size, &err)) {
                    ERR("Formatting unsigned attribute failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                array_size++;       // DW_AT_upper_bound contains highest possible index for array, so size is one more
            }
            break;
        }
    }
    if (DW_DLV_NO_ENTRY != ret && DW_DLV_OK != ret) {
        ERR("Getting DIE failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

    CURSOR_EXEC(insert_array, (ULONG)array_size, unit_id, (ULLONG)offset, basetype);

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (die) {
        dwarf_dealloc(dbg, die, DW_DLA_DIE);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_aggr_type
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for type
 *              tag - DWARF tag for type
 *              unit_id - unit ID
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process aggregate type (structures, unions and enums)
 *
 **************************************************************************/
int proc_aggr_type(Dwarf_Debug dbg, Dwarf_Die parent_die, Dwarf_Half tag, ULONG unit_id) {
    Dwarf_Die       die = NULL, old_die = NULL;
    Dwarf_Off       offset;
    size_t          size = 0;
    char            *name = NULL;
    int             kind;
    Dwarf_Error     err = NULL;
    int             ret = SUCCESS;
    ATTR_LIST(
        ATTR(DW_AT_name,        &name),
        ATTR(DW_AT_byte_size,   &size)
    );

    if (DW_DLV_ERROR == dwarf_dieoffset(parent_die, &offset, &err)) {
        ERR("Getting DIE's offset failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    if (SUCCESS != get_attrs(dbg, parent_die, attr_list)) {
        RETCLEAN(FAILURE);
    }

    /* TODO: classes cannot be anonymous. When classes are implemented, it must be taken into account! */
    if (!ATTR_PRESENT(0)) {
        name = "";        // anonymous struct/union/enum
    }

    switch (tag) {
        case DW_TAG_structure_type:
            kind = TKIND_STRUCT;
            break;
        case DW_TAG_union_type:
            kind = TKIND_UNION;
            break;
        case DW_TAG_enumeration_type:
            kind = TKIND_ENUM;
            break;
        default:        // should never get here
            ERR("Unsupported tag 0x%x", tag);
            RETCLEAN(FAILURE);
    }

    CURSOR_EXEC(insert_type, name, size, kind, unit_id, (ULLONG)offset, 0);

    /* loop through union/struct members or enum items */
    for (ret = dwarf_child(parent_die, &die, &err); DW_DLV_OK == ret; ret = dwarf_siblingof(dbg, old_die, &die, &err)) {

        if (old_die) {
            dwarf_dealloc(dbg, old_die, DW_DLA_DIE);
        }
        old_die = die;

        if (DW_DLV_ERROR == dwarf_tag(die, &tag, &err)) {
            ERR("Getting DIE's tag failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }

        switch (tag) {
            case DW_TAG_member:
                if (SUCCESS != proc_aggr_member(dbg, die, unit_id, offset)) {
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_TAG_enumerator: {
                // do not process enumerators
                break;
            }
            default:
                ERR("Unsupported tag 0x%x for aggregate member (offset 0x%llx)", tag, offset);
                RETCLEAN(FAILURE);
        }
    }

    if (DW_DLV_NO_ENTRY != ret) {
        ERR("Getting DIE failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (die) {
        dwarf_dealloc(dbg, die, DW_DLA_DIE);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_aggr_member
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for type
 *              unit_id - unit ID
 *              offset - offset of parent aggregate type
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process aggregate type (structures, unions) member
 *
 **************************************************************************/
int proc_aggr_member(Dwarf_Debug dbg, Dwarf_Die parent_die, ULONG unit_id, Dwarf_Off offset) {
    int             ret = SUCCESS;
    char            *name = NULL;
    long            start = 0;
    Dwarf_Off       typeref = 0;
    ATTR_LIST(
        ATTR(DW_AT_name,                    &name),
        ATTR(DW_AT_type,                    &typeref),      // reference to type
        ATTR(DW_AT_data_member_location,    &start)
    );

    if (SUCCESS != get_attrs(dbg, parent_die, attr_list)) {
        RETCLEAN(FAILURE);
    }

    CURSOR_EXEC(insert_member, unit_id, (ULLONG)offset, name, (ULLONG)typeref, start);

cleanup:
    cleanup_attrs(dbg, attr_list);

    return ret;
}


/**************************************************************************
 *
 *  Function:   proc_var
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry for type
 *              scope_id - inner-most scope that includes the var
 *              unit_id
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process variables and function params
 *
 **************************************************************************/
int proc_var(Dwarf_Debug dbg, Dwarf_Die die, ULONG scope_id, ULONG unit_id) {
    char            *name = NULL;
    ULONG           typeref = 0;
    ULONG           spec = 0;
    Dwarf_Unsigned  fileno = 0, line;
    Dwarf_Off       offset;
    int             artificial = 0;
    int             global = 0;
    int		        declaration = 0;
    ATTR_LIST(
        ATTR(DW_AT_type,            &typeref),      // reference to type
        ATTR(DW_AT_name,            &name),
        ATTR(DW_AT_artificial,      &artificial),
        ATTR(DW_AT_declaration,	    &declaration),  // declaration-only flag
        ATTR(DW_AT_external,        &global),       // indicate global scope
        ATTR(DW_AT_specification,   &spec),         // offset of specification
        ATTR(DW_AT_decl_file,       &fileno),
        ATTR(DW_AT_decl_line,       &line)          // line info needed for situations when var declared
                                                    // in the middle of scope so we don't want this var to
                                                    // be accessible before its definition
    );
    Dwarf_Error err = NULL;
    int ret = SUCCESS;

    if (DW_DLV_ERROR == dwarf_dieoffset(die, &offset, &err)) {
        ERR("Getting DIE's offset failed - %s", dwarf_errmsg(err));
        RETCLEAN(FAILURE);
    }

    if (SUCCESS != get_attrs(dbg, die, attr_list)) {
        RETCLEAN(FAILURE);
    }

    /* skip artificial and anonymous variables (that aren't definitions for previously declared vars) */
    if (artificial || (!name && !ATTR_PRESENT(5))) {
        RETCLEAN(SUCCESS);
    }

    if (!ATTR_PRESENT(0) && !ATTR_PRESENT(5)) {
        ERR("Missing type for variable %s (offset 0x%llx)", name, offset);
        RETCLEAN(FAILURE);
    }

    if (global) {
        scope_id = 0;   // global scope
//        unit_id = 0;    // global variables aren't unit-specific
    }

    if (ATTR_PRESENT(5)) {      // definition for previous declaration
        CURSOR_EXEC(update_var_loc, fileno, line, unit_id, spec);
    } else {                    // declaraion with or without definition
        /* add new variable */
        CURSOR_EXEC(insert_var, name, unit_id, typeref, scope_id, offset, fileno, line);
    }

cleanup:
    cleanup_attrs(dbg, attr_list);
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   get_attrs
 *
 *  Params:     dbg - debug handle
 *              die - debug info entry
 *              attr_list - list of attributes to get for die
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Get all listed attributes for the die. It attribute is
 *              missing and has mandatory flag, it fails. Each attribute
 *              gets formated according to its form.
 *              Allocates memory for some attributes and fills cleanup
 *              fields with allocation type, suitable for dwarf_dealloc.
 *              Allocated memory must be freed with cleanup_attrs
 *
 **************************************************************************/
int get_attrs(Dwarf_Debug dbg, Dwarf_Die die, struct die_attr *attr_list) {
    int             i;
    Dwarf_Attribute attrib = NULL;
    Dwarf_Half      form;
    Dwarf_Error     err = NULL;
    int             ret = SUCCESS;
 
    for (i = 0; attr_list[i].attr_id; i++) {
        ret = dwarf_attr(die, attr_list[i].attr_id, &attrib, &err);
        if (DW_DLV_ERROR == ret) {
            ERR("Getting attribute failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        } else if (DW_DLV_NO_ENTRY == ret) {
            ret = SUCCESS;
            continue;   // attribute no found - skipping
        }

        attr_list[i].flags |= AF_PRESENT;
        if (DW_DLV_ERROR == dwarf_whatform(attrib, &form, &err)) {
            ERR("Getting attribute form failed - %s", dwarf_errmsg(err));
            RETCLEAN(FAILURE);
        }

        /* populate target variable with attribute value depending on attribute form */
        switch (form) {
            case DW_FORM_string:    /* FALLTHROUGH */
            case DW_FORM_strp:
                if (DW_DLV_ERROR == dwarf_formstring(attrib, (char **)attr_list[i].var, &err)) {
                    ERR("Formatting string attribute failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                attr_list[i].flags |= AF_CLEANUP;
                attr_list[i].cleanup_flag = DW_DLA_STRING;
                break;
            case DW_FORM_udata:
                if (DW_DLV_ERROR == dwarf_formudata(attrib, (Dwarf_Unsigned *)attr_list[i].var, &err)) {
                    ERR("Formatting unsigned attribute failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_FORM_data1:     /* FALLTHROUGH */
            case DW_FORM_data2:     /* FALLTHROUGH */
            case DW_FORM_data4:     /* FALLTHROUGH */
            case DW_FORM_data8:
                /* DW_FORM_dataX form is context-dependend, as per DWARF standard, it can be either signed or unsigned */
                if (DW_AT_decl_line == attr_list[i].attr_id || DW_AT_decl_file == attr_list[i].attr_id || DW_AT_byte_size == attr_list[i].attr_id) {
                    ret = dwarf_formudata(attrib, (Dwarf_Unsigned *)attr_list[i].var, &err);
                } else {
                    ret = dwarf_formsdata(attrib, (Dwarf_Signed *)attr_list[i].var, &err);
                }
                if (DW_DLV_ERROR == ret) {
                    ERR("Formatting data attribute failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_FORM_sdata:
                if (DW_DLV_ERROR == dwarf_formsdata(attrib, (Dwarf_Signed *)attr_list[i].var, &err)) {
                    ERR("Formatting signed attribute failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_FORM_addr:
                if (DW_DLV_ERROR == dwarf_formaddr(attrib, (Dwarf_Addr *)attr_list[i].var, &err)) {
                    ERR("Formatting address failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_FORM_ref1:      /* FALLTHROUGH */
            case DW_FORM_ref2:      /* FALLTHROUGH */
            case DW_FORM_ref4:      /* FALLTHROUGH */
            case DW_FORM_ref8:      /* FALLTHROUGH */
            case DW_FORM_ref_udata: /* FALLTHROUGH */
            case DW_FORM_ref_addr:  /* FALLTHROUGH */
            case DW_FORM_sec_offset:
                // use global offset
                if (DW_DLV_ERROR == dwarf_global_formref(attrib, (Dwarf_Off *)attr_list[i].var, &err)) {
                    ERR("Formatting reference failed (form 0x%x) - %s", form, dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_FORM_flag:      /* FALLTHROUGH */
            case DW_FORM_flag_present:
                if (DW_DLV_ERROR == dwarf_formflag(attrib, (Dwarf_Bool *)attr_list[i].var, &err)) {
                    ERR("Formatting flag failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                break;
            case DW_FORM_block:     /* FALLTHROUGH */
            case DW_FORM_block1:    /* FALLTHROUGH */
            case DW_FORM_block2:    /* FALLTHROUGH */
            case DW_FORM_block4:
                ;
                Dwarf_Block *block;
                if (DW_DLV_ERROR == dwarf_formblock(attrib, &block, &err)) {
                    ERR("Formatting flag failed - %s", dwarf_errmsg(err));
                    RETCLEAN(FAILURE);
                }
                if (block->bl_len > 1 && DW_OP_plus_uconst == *(char *)block->bl_data) {
                    unsigned char byte;
                    ULONG value = 0;
                    for (int i = block->bl_len - 1; i > 0; i--) {
                        byte = ((unsigned char *)block->bl_data)[i];
                        // every byte encodes 7 bits as little-endian
                        value <<= 7;
                        value += byte & 0x7f;
                    }
                    *(long *)attr_list[i].var = value;
                } else {
                    ERR("Unsupported block format (offset %llx)", die_offset(die));
                }
                dwarf_dealloc(dbg, block, DW_DLA_BLOCK);
                break;
            default:
                ERR("Unsupported attribute form 0x%x (offset %llx)", form, die_offset(die));
//                RETCLEAN(FAILURE);
        };
        /* sanity check */
        if (DW_AT_decl_file == attr_list[i].attr_id && *(ULLONG *)attr_list[i].var > (ULLONG)cnt_file) {
            ERR("Decl file ID %ld exceed the count %d at offset 0x%llx", (long)attr_list[i].var, (int)cnt_file, die_offset(die));
            RETCLEAN(MALFUNCTION);
        }
    }

cleanup:
    if (attrib) {
        dwarf_dealloc(dbg, attrib, DW_DLA_ATTR);
    }
    if (err) {
        dwarf_dealloc(dbg, err, DW_DLA_ERROR);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   cleanup_attrs
 *
 *  Params:     dbg - debug handle
 *              attr_list - list of attributes to clean
 *
 *  Return:     N/A
 *
 *  Descr:      Clean all memory that was allocated for attributes
 *              in attr_list
 *
 **************************************************************************/
void cleanup_attrs(Dwarf_Debug dbg, struct die_attr *attr_list) {
    struct die_attr *cur_attr;

    for (cur_attr = attr_list; cur_attr->attr_id; cur_attr++) {
        if ((cur_attr->flags & AF_PRESENT) && (cur_attr->flags & AF_CLEANUP)) {
            dwarf_dealloc(dbg, *(char **)cur_attr->var, cur_attr->cleanup_flag);
            cur_attr->flags = 0;    // prevent segfault on 2nd call
        }
    }

    return;
}


/**************************************************************************
 *
 *  Function:   attr_present
 *
 *  Params:     attr_list - list of attributes to clean
 *              variable-length list of attr_list items required, must
 *              end with negative number
 *
 *  Return:     1 - all required attributes are present
 *              0 - some required attributes are missing
 *
 *  Descr:      Check whether all required attributes are found
 *
 **************************************************************************/
int attr_present(struct die_attr *attr_list, ...) {
    va_list items;
    int     cur_item;

    va_start(items, attr_list);
    for (cur_item = va_arg(items, int); cur_item >= 0; cur_item = va_arg(items, int)) {
        if (!(attr_list[cur_item].flags & AF_PRESENT))
            return 0;
    }
    va_end(items);

    return 1;
}


/**************************************************************************
 *
 *  Function:   die_offset
 *
 *  Params:     die - debug info entry
 *
 *  Return:     offset / 0 on error
 *
 *  Descr:      Return DIE's offset within DWARF structures, useful for
 *              troubleshooting
 *
 **************************************************************************/
Dwarf_Off die_offset(Dwarf_Die die) {
    Dwarf_Error err = NULL;
    Dwarf_Off       offset;

    if (DW_DLV_ERROR == dwarf_dieoffset(die, &offset, &err)) {
        ERR("Getting DIE's offset failed - %s", dwarf_errmsg(err));
        return 0;
    }
    return offset;
}

