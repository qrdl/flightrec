/**************************************************************************
 *
 *  File:       test.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Database access library test ans sample
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include "eel.h"
#include "dab.h"
#include "stingray.h"

#define AUTOINCREMENT "AUTOINCREMENT"

FILE *logfd;

int query(int from, int to) {
    static void *cursor = NULL;
    int ret = DAB_OK;

    if (!cursor) {
        if (DAB_OK != DAB_CURSOR_OPEN(&cursor, "SELECT foo, bar FROM fubar "
                    "WHERE foo >= ? AND foo <= ?", from, to))
            RETCLEAN(DAB_FAIL);
    } else if (DAB_OK != DAB_CURSOR_BIND(cursor, from, to))
        RETCLEAN(DAB_FAIL);

    printf("From %d to %d:\n", from, to);

    int foo;
    sr_string bar = sr_new("", 16);
    while (DAB_OK == (ret = DAB_CURSOR_FETCH(cursor, &foo, bar))) {
        printf("%d => %s\n", foo, CSTR(bar));
    }
    STRFREE(bar);

cleanup:
    if (cursor)
        DAB_CURSOR_RESET(cursor);

    return DAB_NO_DATA == ret ? DAB_OK : ret;
}

void usage(char *prog) {
    printf("Usage: %s -d <database name>\n", prog);
}

int main(int argc, char *argv[]) {
    char *db = NULL;
    int opt;

    logfd = stderr;

    while ((opt = getopt(argc, argv, "d:")) != -1 ) {
        switch (opt) {
            case 'd' : db = optarg;
                       break;
            default:   usage(argv[0]);
                       return EXIT_FAILURE;
        }
    }

    if (!db) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int ret = DAB_OPEN(db, DAB_FLAG_CREATE);
    if (DAB_OK != ret)
        return EXIT_FAILURE;

    if (DAB_OK != DAB_EXEC("CREATE TABLE IF NOT EXISTS fubar ("
                "foo INTEGER PRIMARY KEY " AUTOINCREMENT ", "
                "bar VARCHAR(32))"))
        return EXIT_FAILURE;

    char tmp[] = "foo ";
    int i;
    if (DAB_OK != DAB_BEGIN)
        return EXIT_FAILURE;
    for (i = 0; i < 27; i++) {
        tmp[3] = 'A' + i;
        if (DAB_OK != DAB_EXEC("INSERT INTO fubar (bar) VALUES (?)", tmp)) {
            DAB_ROLLBACK;
            return EXIT_FAILURE;
        }
    }
    if (DAB_OK != DAB_COMMIT)
        return EXIT_FAILURE;

    if (DAB_OK != query(5, 10))
        return EXIT_FAILURE;

    if (DAB_OK != query(15, 20))
        return EXIT_FAILURE;

/*    if (DAB_OK != DAB_EXEC("DROP table fubar"))
        return EXIT_FAILURE; */

    if (DAB_OK != DAB_CLOSE(DAB_FLAG_GRACEFUL))
        return EXIT_FAILURE;

    printf("ok\n");

    return EXIT_SUCCESS;
}

