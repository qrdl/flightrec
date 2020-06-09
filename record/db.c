/**************************************************************************
 *
 *  File:       db.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Database schema operations, and preparing SQL statements
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
#include "stingray.h"
#include "dab.h"
#include "eel.h"

#include "flightrec.h"
#include "record.h"

void *insert_scope = NULL;
void *insert_line = NULL;
void *insert_func = NULL;
void *insert_type = NULL;
void *insert_member = NULL;
void *insert_var = NULL;
void *update_var_loc = NULL;
void *insert_array = NULL;
void *select_type = NULL;
void *select_line = NULL;

/**************************************************************************
 *
 *  Function:   create_db
 *
 *  Params:     N/A
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Create database tables, but not indices. It is much faster
 *              to build indices after tables are filled
 *
 **************************************************************************/
int create_db(void) {
    if (DAB_OK  != DAB_EXEC("CREATE TABLE unit ("
                                "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "name       VARCHAR(255) NOT NULL, "
                                "path       VARCHAR(255) NOT NULL, "
                                "base_addr  INTEGER"
                            ")")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE file ("
                                "id      INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "name    VARCHAR(255) NOT NULL, "
                                "path    VARCHAR(255) NOT NULL, "
                                "unit_id INTEGER NOT NULL, "    // ref unit.id
                                "seq     INTEGER NOT NULL"
                            ")")) {
        return FAILURE;
    }

    // scope doesn't need to reference unit because addresses are unique
    if (DAB_OK != DAB_EXEC("CREATE TABLE scope ("
                                "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "parent     INTEGER, "      // parent scope - scope.id
                                "depth      INTEGER, "      // parent's depth + 1
                                "start_addr INTEGER NOT NULL, "
                                "end_addr   INTEGER NOT NULL"
                            ")")) {
        return FAILURE;
    }

    /* This table is de-normalised to simplify the query executed during runtime analysis */
    /* This table doesn't have primary key, unique index will be added later, after collection */
    if (DAB_OK != DAB_EXEC("CREATE TABLE statement ("
                                "file_id        INTEGER NOT NULL, "     // ref file.id
                                "line           INTEGER NOT NULL, "
                                "address        INTEGER NOT NULL, "
                                "scope_id       INTEGER, "  // ref scope.id
                                "function_id    INTEGER, "  // ref function.id. scope and function may not correspond
                                                            // for lines inside lexical blocks
                                "instr          INTEGER, "  // store original CPU instruction when setting breakpoint
                                "func_flag      INTEGER"    // indicate that this line is the first line of the function
                            ")")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE function ("
                                "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "name       VARCHAR(255) NOT NULL, "
                                "offset     INTEGER NOT NULL, "
                                "scope_id   INTEGER NOT NULL"      // ref scope.id
                            ")")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE type ("
                                "unit_id    INTEGER NOT NULL, "     // ref unit.id
                                "offset     INTEGER NOT NULL, "     // that's how DWARF references parent types
                                "name       VARCHAR(255), "
                                "size       INTEGER DEFAULT 0, "    // size in bytes
                                "dim        INTEGER DEFAULT 0, "    // number of sub-items
                                "parent     INTEGER, "              // referenced type - type.offset
                                "flags      INTEGER NOT NULL, "
                                "indirect   INTEGER DEFAULT 0, "    // number of indirections
                                "PRIMARY KEY (unit_id, offset)"
                            ")")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE member ("
                                "unit_id    INTEGER NOT NULL, "     // ref type.unit_id
                                "offset     INTEGER NOT NULL, "     // ref type.offset - aggregate type
                                "name       VARCHAR(255), "
                                "type       INTEGER, "              // ref type.offset - member type
                                "start      INTEGER, "
                                "value      INTEGER DEFAULT 0, "    // for enum items
                                "PRIMARY KEY (unit_id, offset, name)"
                            ")")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE var ("
                                "id             INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "name           VARCHAR, "
                                "unit_id        INTEGER NOT NULL, "     // ref type.unit_id
                                "type_offset    INTEGER NOT NULL, "     // ref type.offset
                                "scope_id       INTEGER NOT NULL, "     // ref scope.id
                                "offset         INTEGER NOT NULL, "     // needed to match definition to declaration
                                "file_id        INTEGER NOT NULL, "     // ref file.id
                                "line           INTEGER NOT NULL "      // needed to limit var visibility within scope
                            ")")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_EXEC("CREATE TABLE misc ("
                                "key            VARCHAR PRIMARY KEY, "
                                "value"                                 // value can be of any type, so avoid the type
                            ")")) {
        return FAILURE;
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   alter_db
 *
 *  Params:     N/A
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Alter database - create indices, do mass updates.
 *              It is way more effective to make mass updates after DWARF
 *              collection rather than set attributes during the collection
 *              one by one
 *
 **************************************************************************/
int alter_db(void) {

    /* create indices */
    if (DAB_OK != DAB_EXEC("CREATE INDEX scope_addr ON scope ("
                                "start_addr, end_addr)")) {
        return FAILURE;
    }
    if (DAB_OK != DAB_EXEC("CREATE UNIQUE INDEX line ON statement ("
                                "file_id, line, address)")) {
        return FAILURE;
    }
    if (DAB_OK != DAB_EXEC("CREATE INDEX stmt_addr ON statement ("
                                "address)")) {
        return FAILURE;
    }

    /* set innermost scope for each line */
    if (DAB_OK != DAB_EXEC("UPDATE statement SET scope_id = "
            "(SELECT id FROM scope "
                "WHERE "
                    "statement.address < end_addr AND " // end addr is in fact an addr of next scope, so "less than"
                    "statement.address >= start_addr "
                    "order by depth DESC LIMIT 1"
            ")")) {
        return FAILURE;
    }

    /* set function for lines  */
    if (DAB_OK != DAB_EXEC("UPDATE statement SET function_id = "
            "(SELECT f.id FROM "
                    "function f, "
                    "scope s "
                "WHERE "
                    "s.id = f.scope_id AND "
                    "statement.address >= s.start_addr AND "
                    "statement.address < s.end_addr "   // end addr is in fact an addr of next scope, so "less than"
            ")")) {
        return FAILURE;
    }

    /* mark begin-of-function lines  */
    if (DAB_OK != DAB_EXEC("UPDATE statement SET func_flag = "
            "(SELECT 1 FROM "
                    "function f, "
                    "scope s "
                "WHERE "
                    "s.id = f.scope_id AND "
                    "statement.address = s.start_addr"
            ")")) {
        return FAILURE;
    }

    /* mark end-of-function lines  */
    if (DAB_OK != DAB_EXEC("UPDATE statement SET func_flag = 2 "
            "WHERE "
                "rowid IN ("
                    "SELECT "
                        "s.rowid "
                    "FROM "
                        "statement s "
                        "JOIN scope ON "
                            "s.address < scope.end_addr "
                            "AND s.scope_id = scope.id "
                    "WHERE "
                        "s.func_flag IS NULL "
                    "GROUP BY "
                        "s.function_id "
                    "HAVING "
                        "s.address = MAX(s.address)"
                ")")) {
        return FAILURE;
    }

    /* create view for ancestor-descendant relations between types */
    /* for struct, union and array parent type is type of the member/item, for func - return type,
       so parent type not really an ancestor type in such cases */
    if (DAB_OK != DAB_EXEC("CREATE VIEW type_relation AS "
            "WITH RECURSIVE "
                "relation(ancestor, descendant, depth) AS ( "
                "SELECT offset, offset, 0 FROM type "
                "UNION "
                "SELECT parent, descendant, depth+1 FROM type JOIN relation ON offset=relation.ancestor "
            ") "
            "SELECT ancestor, descendant, depth FROM relation")) {
        return FAILURE;
    }

    /* set indirections for pointers */
    if (DAB_OK != DAB_EXEC("UPDATE type SET indirect = 1 "
            "WHERE "
                "(flags & " STR(TKIND_TYPE) ") = " STR(TKIND_POINTER)
                )) {
        return FAILURE;
    }

    // TODO it sets indirecton incorrectly for types, derived from custom pointer types, check impact on expressions
    /* set indirections for types, derived from pointers */
    if (DAB_OK != DAB_EXEC("UPDATE type SET indirect = indirect+1 "
            "WHERE "
                "rowid IN ("
                    "SELECT "
                        "d.rowid "
                    "FROM "
                        "type a "
                        "JOIN type_relation r ON r.ancestor = a.offset "
                        "JOIN type d ON d.offset = r.descendant "
                    "WHERE "
                        "(a.flags & " STR(TKIND_TYPE) ") = " STR(TKIND_POINTER) " AND "
                        "a.rowid != d.rowid"
                ") AND "
                "flags != " STR(TKIND_ALIAS))) {
        return FAILURE;
    }

    /* propagate type definition for derived types from parent type to children */
    if (DAB_OK != DAB_EXEC("UPDATE type SET flags = flags | "
            "(SELECT flags FROM "
                    "type parent "
                "WHERE "
                    "parent.offset = type.parent"
                ")"
            "WHERE "
                "flags & " STR(TKIND_TYPE) " = 0")) {
        return FAILURE;
    }

    /* propagate size for derived types from parent type to children */
    if (DAB_OK != DAB_EXEC("UPDATE type SET size = "
            "(SELECT size FROM "
                    "type parent "
                "WHERE "
                    "parent.offset = type.parent"
                ")"
            "WHERE "
                "size = 0")) {
        return FAILURE;
    }

    /* calculate size for array */
    if (DAB_OK != DAB_EXEC("UPDATE type SET size = dim * "
            "(SELECT size FROM "
                    "type parent "
                "WHERE "
                    "parent.offset = type.parent AND "
                    "parent.size > 0"
                ")"
            "WHERE "
                "size = 0 AND "
                "flags & " STR(TKIND_TYPE) " = " STR(TKIND_ARRAY))) {
        return FAILURE;
    }

    /* calculate number of members for structs */
    if (DAB_OK != DAB_EXEC("UPDATE type SET dim = "
            "(SELECT count(*) FROM "
                    "member "
                "WHERE "
                    "member.offset = type.offset"
                ")"
            "WHERE "
                "flags & " STR(TKIND_TYPE) " IN (" STR(TKIND_STRUCT) "," STR(TKIND_UNION) ")")) {
        return FAILURE;
    }

    /* for all variables replace file sequence (unique within unit) with file ID (unique) */
    if (DAB_OK != DAB_EXEC("UPDATE var SET file_id = "
            "IFNULL((SELECT file.id FROM "
                    "file "
                "WHERE "
                    "file.unit_id = var.unit_id AND "
                    "file.seq = var.file_id"
            "), 0)")) {
        return FAILURE;
    }

    /* global variables are added several times, once for every unit, but only one has a definition,
       so leave only the definition */
    if (DAB_OK != DAB_EXEC("DELETE FROM var "
            "WHERE "
                "scope_id = 0 AND "     // global
                "file_id = 0")) {       // without definition
        return FAILURE;
    }

    /* for scopes add all ancestors */
    if (DAB_OK != DAB_EXEC("CREATE TABLE scope_ancestor AS "
        "WITH RECURSIVE relation(ancestor, descendant) AS ( "
	        "SELECT parent, id FROM scope "
	        "UNION "
	        "SELECT parent, descendant FROM scope JOIN relation ON id=relation.ancestor "
        ") "
        "SELECT descendant as id, ancestor from relation")) {
        return FAILURE;
    }
    if (DAB_OK != DAB_EXEC("CREATE INDEX scope_ancestor_id ON scope_ancestor ("
                                "id)")) {
        return FAILURE;
    }

    /* get function details by scope */
    if (DAB_OK != DAB_EXEC("CREATE VIEW func_for_scope AS SELECT "
                    "a.id as scope_id, "
                    "f.name, "
                    "f.offset "
                "FROM "
                    "scope_ancestor a "
                    "JOIN function f ON f.scope_id = a.ancestor "
                "UNION ALL "
                "SELECT scope_id, name, offset from function")) {
        return FAILURE;
    }

    return SUCCESS;
}

/**************************************************************************
 *
 *  Function:   prepare_everything
 *
 *  Params:     N/A
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Prepare all statements used during debug info collection
 *
 **************************************************************************/
int prepare_statements(void) {
    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_scope, "INSERT "
            "INTO scope "
            "(parent, depth, start_addr, end_addr) VALUES "
            "(?,      ?,     ?,          ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_line, "INSERT "
            "INTO statement "
            "(file_id, line, address) VALUES "
            "(?,       ?,    ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_func, "INSERT "
            "INTO function "
            "(name, scope_id, offset) VALUES "
            "(?,    ?,        ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_type, "INSERT "
            "INTO type "
            "(name, size, flags, unit_id, offset, parent) VALUES "
            "(?,    ?,    ?,     ?,       ?,      ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_member, "INSERT "
            "INTO member "
            "(unit_id, offset, name, type, start, value) VALUES "
            "(?,       ?,      ?,    ?,    ?,     ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_var, "INSERT "
            "INTO var "
            "(name, unit_id, type_offset, scope_id, offset, file_id, line) VALUES "
            "(?,    ?,       ?,           ?,        ?,      ?,       ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&update_var_loc, "UPDATE "
                "var "
            "SET "
                "file_id = ?, "
                "line = ? "
            "WHERE "
                "unit_id = ? AND "
                "offset = ?")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&insert_array, "INSERT "
            "INTO type "
            "(dim, flags,              unit_id, offset, parent) VALUES "
            "(?, " STR(TKIND_ARRAY) ", ?,       ?,      ?)")) {
        return FAILURE;
    }

    if (DAB_OK != DAB_CURSOR_PREPARE(&select_type, "SELECT "
                "size, "
                "parent, "
                "flags "
            "FROM "
                "type "
            "WHERE "
                "unit_id = ? AND "
                "offset = ?")) {
        return FAILURE;
    }

    return SUCCESS;
}

