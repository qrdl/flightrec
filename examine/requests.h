/**************************************************************************
 *
 *  File:       requests.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Debug protocol request handlers
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
#ifndef _REQUESTS_H
#define _REQUESTS_H

#include "jsonapi.h"

enum commands {
    cmd_init = 1,
    cmd_launch,
    cmd_threads,
    cmd_stack,
    cmd_scopes,
    cmd_next,
    cmd_stepin,
    cmd_stepout,
    cmd_stepback,
    cmd_exception_br,
    cmd_config_done,
    cmd_breakpoints,
    cmd_continue,
    cmd_revcontinue,
    cmd_variables,
    cmd_disconnect,
    cmd_evaluate,
};

int process_init(const JSON_OBJ *request, int fd);
int process_launch(const JSON_OBJ *request, int fd);
int process_threads(const JSON_OBJ *request, int fd);
int process_stack(const JSON_OBJ *request, int fd);
int process_scopes(const JSON_OBJ *request, int fd);
int process_next(const JSON_OBJ *request, int fd);
int process_stepin(const JSON_OBJ *request, int fd);
int process_stepout(const JSON_OBJ *request, int fd);
int process_stepback(const JSON_OBJ *request, int fd);
int process_disconnect(const JSON_OBJ *request, int fd);
int process_breakpoints(const JSON_OBJ *request, int fd);
int process_continue(const JSON_OBJ *request, int fd);
int process_revcontinue(const JSON_OBJ *request, int fd);
int process_variables(const JSON_OBJ *request, int fd);
int process_evaluate(const JSON_OBJ *request, int fd);
int process_config_done(const JSON_OBJ *request, int fd);
int just_ack(const JSON_OBJ *request, int fd);

#endif

