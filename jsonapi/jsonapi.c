/**************************************************************************
 *
 *  File:       jsonapi.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      JSON API
 *
 *  Notes:      Main purpose of this file is to define json_err
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
#include "jsonapi.h"

int json_err = JSON_OK;

const char *json_strerror(int err) {
    static char *messages[] = {
        "no error",
        "invalid",
        "path not found",
        "type mismatch"
    };
    if (err < JSON_ERR_MIN || err > JSON_ERR_MAX) {
        return NULL;
    }

    return messages[err];
}

