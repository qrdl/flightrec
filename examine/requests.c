/**************************************************************************
 *
 *  File:       requests.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Processing of DAP protocol requests from debug client
 *
 *  Notes:      https://microsoft.github.io/debug-adapter-protocol/specification
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
#include <math.h>

#include "eel.h"
#include "dab.h"
#include "stingray.h"

#include "examine.h"
#include "requests.h"
#include "jsonapi.h"

#include "expressions/expression.h"

// list of frames - for speed minimise new allocations/deallocations, try to reuse already allocated items
// rewrite of list happens much more often than search
struct frame {
    ULONG           id;
    ULONG           scope;
    ULONG           step;
    ULONG           file;
    ULONG           line;
    struct frame    *next;
} *frame_list = NULL;

static const char *build_response(const JSON_OBJ *request, JSON_OBJ *response, int status, const char *message);
static void event_stopped(const char *reason, int fd);
static void event_inited(int fd);
static void event_terminated(int fd);
static void send_event(JSON_OBJ *evt, const char *type, int fd);

// current execution context
char        *cur_file;
uint64_t    cur_step;
uint64_t    cur_line;
int         cur_depth;

char *source_path = NULL;

// prepared statements, prepared when needed, released by release_cursor()
static void *stack_cursor = NULL;
static void *next_cursor = NULL;
static void *stepin_cursor = NULL;
static void *stepout_cursor = NULL;
static void *stepback_cursor = NULL;
static void *filebypath_cursor = NULL;
static void *addbr_cursor = NULL;
static void *continue_cursor = NULL;
static void *revcontinue_cursor = NULL;
static void *local_vars_cursor = NULL;
static void *global_vars_cursor = NULL;

/**************************************************************************
 *
 *  Function:   process_init
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'initialize' request
 *
 **************************************************************************/
int process_init(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    JSON_OBJ *body = JSON_NEW_OBJ_FIELD(rsp, "body");

    // specify capabilities
#define SET_TRUE(C)  JSON_NEW_TRUE_FIELD(body, #C)
#define SET_FALSE(C)  JSON_NEW_FALSE_FIELD(body, #C)
    // configurationDone is needed to prevent IDE from sending setBreakpoints before the 'launch', when DB is not available
    SET_TRUE(supportsConfigurationDoneRequest);
    SET_TRUE(supportsStepBack);
    SET_TRUE(supportsGotoTargetsRequest);
    SET_TRUE(supportsStepInTargetsRequest);


    SET_FALSE(supportsRestartFrame);    // TODO: to implement

    SET_FALSE(supportsConditionalBreakpoints);
    SET_FALSE(supportsHitConditionalBreakpoints);
    SET_FALSE(supportsDataBreakpoints);
    SET_FALSE(supportsFunctionBreakpoints);
    SET_FALSE(supportsEvaluateForHovers);
    SET_FALSE(supportsSetVariable);
    SET_FALSE(supportsCompletionsRequest);
    SET_FALSE(supportsModulesRequest);
    SET_FALSE(supportsRestartRequest);
    SET_FALSE(supportsExceptionOptions);
    SET_FALSE(supportsValueFormattingOptions);
    SET_FALSE(supportsExceptionInfoRequest);
    SET_FALSE(supportTerminateDebuggee);
    SET_FALSE(supportsDelayedStackTraceLoading);
    SET_FALSE(supportsLoadedSourcesRequest);
    SET_FALSE(supportsLogPoints);
    SET_FALSE(supportsTerminateThreadsRequest);
    SET_FALSE(supportsSetExpression);
    SET_FALSE(supportsTerminateRequest);
    SET_FALSE(supportsReadMemoryRequest);
    SET_FALSE(supportsDisassembleRequest);

    const char *response = build_response(request, rsp, SUCCESS, NULL);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    event_inited(fd);

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   process_launch
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'launch' request, send 'stopped' event
 *
 **************************************************************************/
int process_launch(const JSON_OBJ *request, int fd) {
    int ret = SUCCESS;
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    const char *error;
    const char *response;

    JSON_OBJ *args = JSON_GET_OBJ(request, "arguments");
    if (JSON_OK != json_err) {
        error = "Cannot get 'arguments' param in 'launch' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    const char *program = JSON_GET_STRING_FIELD(args, "program");
    if (JSON_OK != json_err) {
        error = "Cannot get 'program' param in 'launch' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    if (source_path) {
        free(source_path);
    }
    source_path = (char *)JSON_GET_STRING_FIELD(args, "sourcePath");
    if (JSON_OK != json_err) {
        error = "Cannot get 'sourcePath' param in 'launch' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }
    source_path = strdup(source_path);  // need a copy because original becames unusable after JSON object released

    if (SUCCESS != open_dbginfo(program)) {
        error = "Cannot read debug info";
        RETCLEAN(FAILURE);
    }

    // read optional param for DB path
    char *db_name = (char *)JSON_GET_STRING_FIELD(args, "collectedData");
    if (JSON_OK != json_err) {
        db_name = malloc(strlen(program) + 4);
        strcpy(db_name, program);
        strcat(db_name, ".fr");
    }

    if (DAB_OK != DAB_OPEN(db_name, DAB_FLAG_NONE)) {
        error = "Cannot open database file";    // error is logged by DAB_OPEN()
        RETCLEAN(FAILURE);
    }
    if (JSON_OK != json_err) {
        free(db_name);
    }

    // attach in-memory DB and create 'breakpoint' table
    if (DAB_OK != DAB_EXEC("ATTACH ':memory:' AS local")) {
        error = "Cannot create local cache";    // error is logged by DAB_EXEC()
        RETCLEAN(FAILURE);
    }
    if (DAB_OK != DAB_EXEC( "CREATE TABLE local.breakpoint("
                                "id      INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "file_id INTEGER NOT NULL, "
                                "line    INTEGER NOT NULL"
                            ")")) {
        error = "Cannot create breakpoint table";    // error is logged by DAB_EXEC()
        RETCLEAN(FAILURE);
    }
    if (DAB_OK != DAB_EXEC("CREATE UNIQUE INDEX local.br_by_line ON breakpoint ("
                                "file_id, line)")) {
        error = "Cannot create breakpoint index";    // error is logged by DAB_EXEC()
        RETCLEAN(FAILURE);
    }

    // get first step from DB and set it as current step
    void *cursor;
    int db_err = DAB_CURSOR_OPEN(&cursor, "SELECT "
                "f.name, "
                "s.line "
            "FROM "
                "file f JOIN "
                "step s ON "
                    "f.id = s.file_id "
            "WHERE "
                "s.id = 1");
    if (DAB_OK != db_err) {
        error = "Cannot query database";    // error is logged by DAB_CURSOR_OPEN()
        RETCLEAN(FAILURE);
    }

    db_err = DAB_CURSOR_FETCH(cursor, &cur_file, &cur_line);
    DAB_CURSOR_FREE(cursor);
    if (DAB_NO_DATA == db_err) {
        error = "DB doesn't contain execution info";
        ERR(error);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != db_err) {
        error = "Cannot get step info from DB";     // error logged by FETCH
        RETCLEAN(FAILURE);
    }
    cur_step = 1;
    cur_depth = 1;

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);

    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("entry", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_threads
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'threads' request
 *
 **************************************************************************/
int process_threads(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error = NULL;

    // TODO: add real support for threads
    JSON_OBJ *threads = JSON_NEW_ARRAY_FIELD(JSON_NEW_OBJ_FIELD(rsp, "body"), "threads");
    JSON_OBJ *item = JSON_NEW_OBJ();
    JSON_NEW_STRING_FIELD(item, "name", "thread 1");
    JSON_NEW_INT32_FIELD(item, "id", 1);
    JSON_ADD_OBJ_ITEM(threads, item);

    const char *response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_stack
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'stack' request
 *
 **************************************************************************/
int process_stack(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error = NULL;
    const char *response;

    JSON_OBJ *args = JSON_GET_OBJ(request, "arguments");
    if (JSON_OK != json_err) {
        error = "Missing 'arguments' param in 'stackTrace' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    unsigned int skip_frames = (unsigned int)JSON_GET_INT32_FIELD(args, "startFrame");
    if (JSON_OK != json_err) {
        error = "Invalid 'startFrame' param in 'stackTrace' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }
    unsigned int max_frames = (unsigned int)JSON_GET_INT32_FIELD(args, "levels");
    if (JSON_OK != json_err) {
        error = "Invalid 'levels' param in 'stackTrace' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    if (!stack_cursor) {
        // TODO: Probably recursive query is faster, but I didn't figure it out yet
        if (DAB_OK != DAB_CURSOR_OPEN(&stack_cursor,
            "SELECT DISTINCT "       // distinct is required not to get duplicate records when more then one statement on the line
                "f.id, "
                "f.name, "
                "f.path, "
                "s.line, "
                "fun.name, "
                "st.scope_id, "
                "s.id "
            "FROM "
                "step s "
                "JOIN file f ON "
                    "f.id = s.file_id "
                "JOIN function fun ON "
                    "fun.id = s.function_id "
                "JOIN statement st ON "
                    "st.file_id = s.file_id AND "
                    "st.line = s.line "
            "WHERE "
                "s.id IN ("
                    "SELECT MAX(id) FROM step WHERE id <= ? AND depth <= ? GROUP BY depth"     // TODO: may benefit from index by id + depth
                ")"
            "ORDER BY "
                "s.depth DESC",
            cur_step, cur_depth
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(stack_cursor) || DAB_OK != DAB_CURSOR_BIND(stack_cursor, cur_step, cur_depth)) {
        error = "Cannot query stack trace";
        RETCLEAN(FAILURE);
    }

    JSON_OBJ *body = JSON_NEW_OBJ_FIELD(rsp, "body");
    JSON_OBJ *frames = JSON_NEW_ARRAY_FIELD(body, "stackFrames");

    char *filename, *fun_name, *path;
    ULONG line, file_id, scope_id, step_id, id = 0;
    struct frame *prev_frame = NULL, *cur_frame = frame_list;
    int db_err;
    while (DAB_OK == (db_err = DAB_CURSOR_FETCH(stack_cursor, &file_id, &filename, &path, &line, &fun_name, &scope_id, &step_id))) {
        // add entry to frame list
        if (!cur_frame) {
            cur_frame = malloc(sizeof(*cur_frame));
            cur_frame->next = NULL;
            if (prev_frame) {
                prev_frame->next = cur_frame;
            } else {
                frame_list = cur_frame;     // top item
            }
        }
        cur_frame->id = id;
        cur_frame->scope = scope_id;
        cur_frame->step = step_id;
        cur_frame->file = file_id;
        cur_frame->line = line;
        prev_frame = cur_frame;
        cur_frame = cur_frame->next;

        if (id < skip_frames) {
            id++;
            continue;   // skip frame
        }
        if (id - skip_frames >= max_frames) {
            break;      // max level of frames reached
        }

        char *fullpath = malloc(strlen(source_path) + strlen(path) + 2);
        sprintf(fullpath, "%s/%s", source_path, path);
        JSON_OBJ *item = JSON_ADD_NEW_ITEM(frames);
        JSON_NEW_STRING_FIELD(item, "name", fun_name);
        JSON_NEW_INT64_FIELD(item, "id", id++);
        JSON_NEW_INT64_FIELD(item, "line", line);
        JSON_NEW_INT32_FIELD(item, "column", 0);
        JSON_OBJ *source = JSON_NEW_OBJ_FIELD(item, "source");
        JSON_NEW_STRING_FIELD(source, "name", filename);
        JSON_NEW_STRING_FIELD(source, "path", fullpath);
        JSON_NEW_INT32_FIELD(source, "sourceReference", 0);  // 0 means IDE must retrieve sources itself
    }
    JSON_NEW_INT64_FIELD(body, "totalFrames", id - skip_frames);
    if (DAB_NO_DATA != db_err) {
        error = "Error fetching frames";
        ERR(error);     // detailed error logged by FETCH
        RETCLEAN(FAILURE);
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_next
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'next' request, send 'stopped' event
 *
 **************************************************************************/
int process_next(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    if (!next_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&next_cursor,
            "SELECT "
                "f.name, "
                "s.line, "
                "s.id, "
                "s.depth "
            "FROM "
                "file f JOIN "
                "step s ON "
                    "f.id = s.file_id "
            "WHERE "
                "s.id > ? AND "
                "s.depth <= ?"
            "ORDER BY "
                "s.id "
            "LIMIT 1",
            cur_step, cur_depth
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(next_cursor) || DAB_OK != DAB_CURSOR_BIND(next_cursor, cur_step, cur_depth)) {
        error = "Cannot query next step";
        RETCLEAN(FAILURE);
    }

    ret = DAB_CURSOR_FETCH(next_cursor, &cur_file, &cur_line, &cur_step, &cur_depth);
    if (DAB_NO_DATA == ret) {
        event_terminated(fd);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != ret) {
        ERR("Cannot get next step");
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("step", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_stepin
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'stepIn' request, send 'stopped' event
 *
 **************************************************************************/
int process_stepin(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    if (!stepin_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&stepin_cursor,
            "SELECT "
                "f.name, "
                "s.line, "
                "s.id, "
                "s.depth "
            "FROM "
                "file f JOIN "
                "step s ON "
                    "f.id = s.file_id "
            "WHERE "
                "s.id = ? + 1",
            cur_step
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(stepin_cursor) || DAB_OK != DAB_CURSOR_BIND(stepin_cursor, cur_step)) {
        error = "Cannot query next step";
        RETCLEAN(FAILURE);
    }

    ret = DAB_CURSOR_FETCH(stepin_cursor, &cur_file, &cur_line, &cur_step, &cur_depth);
    if (DAB_NO_DATA == ret) {
        event_terminated(fd);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != ret) {
        ERR("Cannot get next step");
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("step", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_stepout
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'stepOut' request, send 'stopped' event
 *
 **************************************************************************/
int process_stepout(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    if (cur_depth <= 1) {   // do nothing - already at top level
        RETCLEAN(SUCCESS);
    }

    if (!stepout_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&stepout_cursor,
            "SELECT "
                "f.name, "
                "s.line, "
                "s.id, "
                "s.depth "
            "FROM "
                "file f JOIN "
                "step s ON "
                    "f.id = s.file_id "
            "WHERE "
                "s.id > ? AND "
                "s.depth < ?"
            "ORDER BY "
                "s.id "
            "LIMIT 1",
            cur_step, cur_depth
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(stepout_cursor) || DAB_OK != DAB_CURSOR_BIND(stepout_cursor, cur_step, cur_depth)) {
        error = "Cannot query next step";
        RETCLEAN(FAILURE);
    }

    ret = DAB_CURSOR_FETCH(stepout_cursor, &cur_file, &cur_line, &cur_step, &cur_depth);
    if (DAB_NO_DATA == ret) {
        event_terminated(fd);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != ret) {
        ERR("Cannot get next step");
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("step", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_stepback
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'stepBack' request, send 'stopped' event
 * 
 *  Note:       StepBack jumps over function calls like 'next', but in
 *              reverse direction
 *
 **************************************************************************/
int process_stepback(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    if (cur_step <= 1) {
        RETCLEAN(SUCCESS);  // already at first step - nothing to do
    }

    if (!stepback_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&stepback_cursor,
            "SELECT "
                "f.name, "
                "s.line, "
                "s.id, "
                "s.depth "
            "FROM "
                "file f JOIN "
                "step s ON "
                    "f.id = s.file_id "
            "WHERE "
                "s.id < ? AND "
                "s.depth <= ?"
            "ORDER BY "
                "s.id DESC "
            "LIMIT 1",
            cur_step, cur_depth
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(stepback_cursor) || DAB_OK != DAB_CURSOR_BIND(stepback_cursor, cur_step, cur_depth)) {
        error = "Cannot query next step";
        RETCLEAN(FAILURE);
    }

    ret = DAB_CURSOR_FETCH(stepback_cursor, &cur_file, &cur_line, &cur_step, &cur_depth);
    if (DAB_NO_DATA == ret) {
        event_terminated(fd);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != ret) {
        ERR("Cannot get next step");
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("step", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_breakpoints
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'setBreakpoints' request
 *
 **************************************************************************/
int process_breakpoints(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    // find source file
    JSON_OBJ *args = JSON_GET_OBJ(request, "arguments");
    if (JSON_OK != json_err) {
        error = "Cannot find 'arguments' param in 'setBreakpoints' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }
    
    const char *fname = JSON_GET_STRING_FIELD(JSON_GET_OBJ(args, "source"), "name");
    if (JSON_OK != json_err) {
        error = "Cannot get 'source/name' param in 'setBreakpoints' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }
    // TODO: There is a risk to find the wrong file just by name. Add path processing

    if (!filebypath_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&filebypath_cursor,
            "SELECT "
                "id "
            "FROM "
                "file "
            "WHERE "
                "name = ?",
            fname
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(filebypath_cursor) || DAB_OK != DAB_CURSOR_BIND(filebypath_cursor, fname)) {
        error = "Cannot query file";
        RETCLEAN(FAILURE);
    }

    uint64_t fid;
    if (DAB_OK != DAB_CURSOR_FETCH(filebypath_cursor, &fid)) {
        error = "Cannot query file";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    // clear all breakpoint previously set in this source
    if (DAB_OK != DAB_EXEC("DELETE FROM local.breakpoint WHERE file_id = ?", fid)) {
        error = "Cannot clear old breakpoints";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    JSON_OBJ *br = JSON_GET_ARRAY(args, "breakpoints");
    if (JSON_OK != json_err) {
        error = "Cannot find 'breakpoints' param in 'setBreakpoints' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    // TODO: Instead of rejecting non-stoppable breakpoints, move them to next stoppable line

    if (!addbr_cursor &&
        // do insert as SELECT to verify that this line is really stoppable
        DAB_OK != DAB_CURSOR_PREPARE(&addbr_cursor,
            "INSERT "
                "INTO local.breakpoint "
                "(file_id, line) "
            "SELECT "
                " file_id, line "
            "FROM "
                "main.statement "
            "WHERE "
                "file_id = ? AND "
                "line = ? "
            "LIMIT 1"           // need to limit because there could be more than one statement on the line
        )) {
        error = "Cannot prepare statement";
        RETCLEAN(FAILURE);
    }

    JSON_OBJ *out_br = JSON_NEW_ARRAY_FIELD(JSON_NEW_OBJ_FIELD(rsp, "body"), "breakpoints");

    // loop through lines and add breakpoints
    JSON_OBJ *item;
    for (int i = 0; NULL != (item = JSON_GET_ITEM(br, i)); i++) {
        int line = JSON_GET_INT32_FIELD(item, "line");
        if (JSON_OK != json_err) {
            ERR("Invalid or missing 'line' for breakpoint %d", i);
            RETCLEAN(FAILURE);
        }
        if (DAB_OK != DAB_CURSOR_RESET(addbr_cursor) || DAB_OK != DAB_CURSOR_BIND(addbr_cursor, fid, line)) {
            error = "Cannot cache breakpoint";
            RETCLEAN(FAILURE);
        }
        if (DAB_NO_DATA != DAB_CURSOR_FETCH(addbr_cursor)) {
            error = "Cannot cache breakpoint";
            RETCLEAN(FAILURE);
        }
        item = JSON_ADD_NEW_ITEM(out_br);
        if (!DAB_AFFECTED_ROWS) {   
            // file and line don't correspond to stoppable location - breakpoint not verified
            JSON_NEW_FALSE_FIELD(item, "verified");
        } else {
            JSON_NEW_TRUE_FIELD(item, "verified");
            JSON_NEW_INT32_FIELD(item, "id", DAB_LAST_ID);
        }
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_continue
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'continue' request
 *
 **************************************************************************/
int process_continue(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    if (!continue_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&continue_cursor,
            "SELECT "
                "f.name, "
                "s.line, "
                "s.id, "
                "s.depth "
            "FROM "
                "file f "
                "JOIN step s ON "
                    "f.id = s.file_id "
                "JOIN local.breakpoint br ON "
                    "br.file_id = f.id AND "
                    "br.line = s.line "
            "WHERE "
                "s.id > ? "
            "ORDER BY "
                "s.id",
            cur_step
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(continue_cursor) || DAB_OK != DAB_CURSOR_BIND(continue_cursor, cur_step)) {
        error = "Cannot query next breakpoint";
        RETCLEAN(FAILURE);
    }

    ret = DAB_CURSOR_FETCH(continue_cursor, &cur_file, &cur_line, &cur_step, &cur_depth);
    if (DAB_NO_DATA == ret) {
        event_terminated(fd);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != ret) {
        ERR("Cannot get next step");
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("breakpoint", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_revcontinue
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'reverseContinue' request
 *
 **************************************************************************/
int process_revcontinue(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error = NULL;
    const char *response;

    if (!revcontinue_cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&revcontinue_cursor,
            "SELECT "
                "f.name, "
                "s.line, "
                "s.id, "
                "s.depth "
            "FROM "
                "file f "
                "JOIN step s ON "
                    "f.id = s.file_id "
                "JOIN local.breakpoint br ON "
                    "br.file_id = f.id AND "
                    "br.line = s.line "
            "WHERE "
                "s.id < ? "
            "ORDER BY "
                "s.id DESC",
            cur_step
        )) {
            error = "Cannot prepare statement";
            RETCLEAN(FAILURE);
        }
    } else if (DAB_OK != DAB_CURSOR_RESET(revcontinue_cursor) || DAB_OK != DAB_CURSOR_BIND(revcontinue_cursor, cur_step)) {
        error = "Cannot query next breakpoint";
        RETCLEAN(FAILURE);
    }

    ret = DAB_CURSOR_FETCH(revcontinue_cursor, &cur_file, &cur_line, &cur_step, &cur_depth);
    if (DAB_NO_DATA == ret) {
        event_terminated(fd);
        RETCLEAN(FAILURE);
    } else if (DAB_OK != ret) {
        ERR("Cannot get next step");
        RETCLEAN(FAILURE);
    } else {
        ret = SUCCESS;
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    if (SUCCESS == ret) {
        event_stopped("breakpoint", fd);
    }

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_scopes
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'scopes' request. Return two scopes: global
 *              one and the local one, where local is inner-most scope
 *              for current step
 *
 **************************************************************************/
int process_scopes(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    int frame_id = JSON_GET_INT32_FIELD(JSON_GET_OBJ(request, "arguments"), "frameId");
    if (JSON_OK != json_err) {
        error = "Cannot find 'arguments/frameId' param in 'scopes' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    JSON_OBJ *scopes = JSON_NEW_ARRAY_FIELD(JSON_NEW_OBJ_FIELD(rsp, "body"), "scopes");

    // look for frame in frame list
    struct frame *cur_frame;
    for (cur_frame = frame_list; cur_frame; cur_frame = cur_frame->next) {
        if (cur_frame->id == (unsigned)frame_id) {
            break;
        }
    }
    if (!cur_frame) {
        error = "Unknown frame in 'scopes' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    /* VSCode doesn't support 0 and negative var references, so I use non-integer references to identify scopes */
    JSON_OBJ *item = JSON_ADD_NEW_ITEM(scopes);
    JSON_NEW_STRING_FIELD(item, "name", "Globals");
    JSON_NEW_STRING_FIELD(item, "presentationHint", "globals");
    JSON_NEW_DBL_FIELD(item, "variablesReference", GLOBAL_SCOPE + 0.5);

    item = JSON_ADD_NEW_ITEM(scopes);
    JSON_NEW_STRING_FIELD(item, "name", "Locals");
    JSON_NEW_STRING_FIELD(item, "presentationHint", "locals");
    JSON_NEW_DBL_FIELD(item, "variablesReference", cur_frame->scope + 0.5);

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return ret;
}


/**************************************************************************
 *
 *  Function:   process_variables
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'variables' request.
 *
 **************************************************************************/
int process_variables(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    const char *error;
    const char *response;

    JSON_OBJ *req = JSON_GET_OBJ(request, "arguments");
    if (JSON_OK != json_err) {
        error = "Cannot find 'argument' param in 'variables' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }
    ULONG var_ref = 0;
    // integer value means var reference, non-integer value means scope
    double scope = JSON_GET_DBL_FIELD(req, "variablesReference");
    if (JSON_ERR_MISMATCH == json_err) {
        var_ref = JSON_GET_INT64_FIELD(req, "variablesReference");
    }
    if (JSON_OK != json_err) {
        error = "Invalid 'variablesReference' param in 'variables' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    JSON_OBJ *vars = JSON_NEW_ARRAY_FIELD(JSON_NEW_OBJ_FIELD(rsp, "body"), "variables");

    if (scope) {  // non-integer value means scope
        if (GLOBAL_SCOPE == (int)scope) {
            // TODO separate static vars from globals using unit_id
            if (!global_vars_cursor) {
                if (DAB_OK != DAB_CURSOR_OPEN(&global_vars_cursor,
                    "SELECT "
                        "v.id "
                    "FROM "
                        "var v "
                    "WHERE "
                        "v.scope_id = 0"
                )) {
                    error = "Cannot prepare statement";
                    RETCLEAN(FAILURE);
                }
            } else if (DAB_OK != DAB_CURSOR_RESET(global_vars_cursor)) {
                error = "Cannot query next breakpoint";
                RETCLEAN(FAILURE);
            }

            int db_err;
            ULONG id;
            while (DAB_OK == (db_err = DAB_CURSOR_FETCH(global_vars_cursor, &id))) {
                if (SUCCESS != add_var(GLOBAL_SCOPE, vars, id, cur_step)) {
                    error = "Error adding variable";
                    ERR(error);
                    RETCLEAN(FAILURE);
                }
            }
            if (DAB_NO_DATA != db_err) {
                error = "Error fetching variables";
                ERR(error);     // detailed error logged by FETCH
                RETCLEAN(FAILURE);
            }
        } else {
            // look for frame in frame list to know the step
            struct frame *cur_frame;
            for (cur_frame = frame_list; cur_frame; cur_frame = cur_frame->next) {
                if (cur_frame->scope == (unsigned int)scope) {
                    break;
                }
            }
            if (!cur_frame) {
                error = "Unknown variablesReference in 'variables' request";
                ERR(error);
                RETCLEAN(FAILURE);
            }

            if (!local_vars_cursor) {
                if (DAB_OK != DAB_CURSOR_OPEN(&local_vars_cursor,
                    "SELECT "
                        "v.id, "
                        "v.file_id, "
                        "v.line "
                    "FROM "
                        "var v, "
                        "scope_ancestor s "
                    "WHERE "
                        "("
                            "v.scope_id = s.ancestor "      // var from ancestor scope
                            "OR "
                            "v.scope_id = ?"                // or from current scope
                        ") AND "
                        "s.id = ? AND "
                        "v.scope_id != " STR(GLOBAL_SCOPE),                 // except global scope
                    cur_frame->scope, cur_frame->scope
                )) {
                    error = "Cannot prepare statement";
                    RETCLEAN(FAILURE);
                }
            } else if (DAB_OK != DAB_CURSOR_RESET(local_vars_cursor) || DAB_OK != DAB_CURSOR_BIND(local_vars_cursor, cur_frame->scope, cur_frame->scope)) {
                error = "Cannot query next breakpoint";
                RETCLEAN(FAILURE);
            }

            int db_err;
            ULONG id, file, line;
            while (DAB_OK == (db_err = DAB_CURSOR_FETCH(local_vars_cursor, &id, &file, &line))) {
                if (file == cur_frame->file && line >= cur_frame->line) {
                    continue;   // var is not dectared yet
                }
                if (SUCCESS != add_var((unsigned int)scope, vars, id, cur_frame->step)) {
                    error = "Error adding variable";
                    ERR(error);
                    RETCLEAN(FAILURE);
                }
            }
            if (DAB_NO_DATA != db_err) {
                error = "Error fetching variables";
                ERR(error);     // detailed error logged by FETCH
                RETCLEAN(FAILURE);
            }
        }
    } else {        // reference is variable/item/field
        const char *filter = JSON_GET_STRING_FIELD(req, "filter");
        
        if (JSON_OK == json_err && !strcmp(filter, "indexed")) {
            /* add array items */
            unsigned int start = JSON_GET_INT32_FIELD(req, "start");
            if (JSON_OK != json_err) {
                error = "Invalid 'start' param in 'variables' request";
                ERR(error);
                RETCLEAN(FAILURE);
            }
            unsigned int count = JSON_GET_INT32_FIELD(req, "count");
            if (JSON_OK != json_err) {
                error = "Invalid 'start' param in 'variables' request";
                ERR(error);
                RETCLEAN(FAILURE);
            }

            add_var_items(vars, var_ref, start, count);
        } else {
            /* add struct fields */
            add_var_fields(vars, var_ref);
        }
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return ret;
}


/**************************************************************************
 *
 *  Function:   just_ack
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Just acknowledge the request, ignoring it
 *
 **************************************************************************/
int just_ack(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();

    const char *response = build_response(request, rsp, SUCCESS, NULL);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   process_evaluate
 *
 *  Params:     request - JSON request
 *              fd - descriptior to send response to
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process IDE's 'evaluate' request.
 *
 **************************************************************************/
int process_evaluate(const JSON_OBJ *request, int fd) {
    JSON_OBJ *rsp = JSON_NEW_OBJ();
    int ret = SUCCESS;
    char *error = NULL;
    const char *response;
    struct ast_node *ast = NULL;

    JSON_OBJ *req = JSON_GET_OBJ(request, "arguments");
    if (JSON_OK != json_err) {
        error = "Cannot find 'argument' param in 'evaluate' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    const char *expr_text = JSON_GET_STRING_FIELD(req, "expression");
    if (JSON_OK != json_err) {
        error = "Cannot get 'expression' param in 'evaluate' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    uint64_t frame_id = JSON_GET_INT64_FIELD(req, "frameId");
    uint64_t scope = GLOBAL_SCOPE;
    uint64_t step = cur_step;
    if (JSON_OK == json_err) {
        /* find the scope for the frame specified */
        struct frame *cur_frame;
        for (cur_frame = frame_list; cur_frame; cur_frame = cur_frame->next) {
            if (cur_frame->id == (unsigned)frame_id) {
                break;
            }
        }
        if (!cur_frame) {
            error = "Unknown 'frameId' specified in 'evaluate' request";
            ERR(error);
            RETCLEAN(FAILURE);
        }
        scope = cur_frame->scope;
        step = cur_frame->step;
    } else if (JSON_OK != json_err) {
        error = "Cannot get 'frameId' param in 'evaluate' request";
        ERR(error);
        RETCLEAN(FAILURE);
    }

    uint64_t id;
    if (SUCCESS != query_expr_cache(expr_text, &id, &ast)) {
        error = "Cannot read expression cache";
        ERR(error);
        RETCLEAN(FAILURE);
    }
    if (!ast) {
        ast = expr_parse(expr_text, scope, &error);
        if (error) {
            ERR(error);
            RETCLEAN(FAILURE);
        }
        if (SUCCESS != update_expr_cache(id, ast)) {
            free_ast_node(ast);
            error = "Cannot update expression cache";
            ERR(error);
            RETCLEAN(FAILURE);
        }
    }

    JSON_OBJ *body = JSON_NEW_OBJ_FIELD(rsp, "body");
    if (FAILURE == get_eval_result(body, id, ast, step, &error)) {
        RETCLEAN(FAILURE);
    }

cleanup:
    response = build_response(request, rsp, ret, SUCCESS == ret ? NULL : error);
    int err = send_message(fd, response);
    if (SUCCESS != err) {
        ERR("Cannot send response");
        return FAILURE;
    }
    JSON_RELEASE(rsp);

    return ret;
}


/**************************************************************************
 *
 *  Function:   build_response
 *
 *  Params:     request - JSON request
 *              response - JSON response
 *              status - processing status, SUCCESS / FAILURE
 *              message - error message (used when status == FAILURE)
 *
 *  Return:     serialised JSON response
 *
 *  Descr:      Add common reponse fields
 *
 **************************************************************************/
const char *build_response(const JSON_OBJ *request, JSON_OBJ *response, int status, const char *message) {   
    JSON_NEW_STRING_FIELD(response, "type", "response");

    // copy command and sequence from request
    JSON_OBJ *item = JSON_COPY_OBJ(JSON_GET_OBJ(request, "command"));
    JSON_ADD_OBJ_FIELD(response, "command", item);
    item = JSON_COPY_OBJ(JSON_GET_OBJ(request, "seq"));
    JSON_ADD_OBJ_FIELD(response, "request_seq", item);
    JSON_NEW_INT32_FIELD(response, "seq", 0);

    // set status and optional message
    if (SUCCESS == status) {
        JSON_NEW_TRUE_FIELD(response, "success");
    } else {
        JSON_NEW_FALSE_FIELD(response, "success");
        if (message) {
            JSON_NEW_STRING_FIELD(response, "message", message);
        }
    }

    // serialise
    return JSON_PRINT(response);
}


/**************************************************************************
 *
 *  Function:   event_stopped
 *
 *  Params:     reason - stop reason
 *              fd - file descriptor to write event to
 *
 *  Return:     N/A
 *
 *  Descr:      Send 'stopped' event with specified reason
 *
 **************************************************************************/
void event_stopped(const char *reason, int fd) {
    JSON_OBJ *evt = JSON_NEW_OBJ();

    JSON_OBJ *body = JSON_NEW_OBJ_FIELD(evt, "body");
    JSON_NEW_STRING_FIELD(body, "reason", reason);
    JSON_NEW_INT32_FIELD(body, "threadId", 1);

    send_event(evt, "stopped", fd);
    JSON_RELEASE(evt);
}


/**************************************************************************
 *
 *  Function:   event_inited
 *
 *  Params:     reason - stop reason
 *              fd - file descriptor to write event to
 *
 *  Return:     N/A
 *
 *  Descr:      Send 'initialized' event with specified reason
 *
 **************************************************************************/
void event_inited(int fd) {
    JSON_OBJ *evt = JSON_NEW_OBJ();
    send_event(evt, "initialized", fd);
    JSON_RELEASE(evt);
}


/**************************************************************************
 *
 *  Function:   event_terminated
 *
 *  Params:     reason - stop reason
 *              fd - file descriptor to write event to
 *
 *  Return:     N/A
 *
 *  Descr:      Send 'terminated' event with specified reason
 *
 **************************************************************************/
void event_terminated(int fd) {
    JSON_OBJ *evt = JSON_NEW_OBJ();
    send_event(evt, "terminated", fd);
    JSON_RELEASE(evt);
}


/**************************************************************************
 *
 *  Function:   send_event
 *
 *  Params:     evt - event JSON
 *              type - event type
 *              fd - file descriptor to write event to
 *
 *  Return:     N/A
 *
 *  Descr:      Send 'initialized' event with specified reason
 *
 **************************************************************************/
void send_event(JSON_OBJ *evt, const char *type, int fd) {
    JSON_NEW_STRING_FIELD(evt, "type", "event");
    JSON_NEW_STRING_FIELD(evt, "event", type);
    JSON_NEW_INT32_FIELD(evt, "seq", 0);

    const char *message = JSON_PRINT(evt);

    int err = send_message(fd, message);
    if (SUCCESS != err) {
        ERR("Cannot send event");
    }
}


/**************************************************************************
 *
 *  Function:   release_cursors
 *
 *  Params:     N/A
 *
 *  Return:     N/A
 *
 *  Descr:      Release all cursors that were possibly allocated during
 *              debug section
 *
 **************************************************************************/
void release_cursors(void) {
    DAB_CURSOR_FREE(stack_cursor);
    DAB_CURSOR_FREE(next_cursor);
    DAB_CURSOR_FREE(stepin_cursor);
    DAB_CURSOR_FREE(stepout_cursor);
    DAB_CURSOR_FREE(stepback_cursor);
    DAB_CURSOR_FREE(filebypath_cursor);
    DAB_CURSOR_FREE(addbr_cursor);
    DAB_CURSOR_FREE(continue_cursor);
    DAB_CURSOR_FREE(revcontinue_cursor);
    DAB_CURSOR_FREE(local_vars_cursor);
    DAB_CURSOR_FREE(global_vars_cursor);

    // statements from var_value.c
    DAB_CURSOR_FREE(var_cursor);
    DAB_CURSOR_FREE(step_cursor);
    DAB_CURSOR_FREE(array_cursor);
    DAB_CURSOR_FREE(struct_cursor);
    DAB_CURSOR_FREE(member_cursor);
    DAB_CURSOR_FREE(mem_cursor);
    DAB_CURSOR_FREE(type_cursor);
    DAB_CURSOR_FREE(ref_cursor);
    DAB_CURSOR_FREE(ref_upsert);
    DAB_CURSOR_FREE(heap_cursor);
    DAB_CURSOR_FREE(func_cursor);
    DAB_CURSOR_FREE(type_name_cursor);

    void close_expr_cursors();
}

