/**************************************************************************
 *
 *  File:       record.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      'record' entry point
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
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <libgen.h>
#include <linux/limits.h>

#include "stingray.h"
#include "dab.h"
#include "eel.h"

#include "flightrec.h"
#include "record.h"

static void print_usage(char *name);

FILE            *logfd;
char            *acceptable_path;
struct entry    *ignore_unit;
struct entry    *process_unit;

char *db_name;  // DB file name used by workers
uid_t uid;
gid_t gid;

/**************************************************************************
 *
 *  Function:   main
 *
 *  Params:     standard
 *
 *  Return:     EXIT_SUCCESS / EXIT_FAILURE
 *
 *  Descr:      Entry point
 *
 **************************************************************************/
int main(int argc, char *argv[]) {
    logfd = stderr;
    int c;

    while ((c = getopt(argc, argv, "p:x:i:l:")) != -1) {
        if ('p' == c) {
            acceptable_path = optarg;
        } else if ('l' == c) {
            FILE *tmp = fopen(optarg, "w");
            if (!tmp) {
                fprintf(stderr, "Cannot open log file '%s' : %s", optarg, strerror(errno));
                return EXIT_FAILURE;
            }
            logfd = tmp;
        } else if ('x' == c) {
            struct entry *tmp = malloc(sizeof(*tmp));
            tmp->name = strdup(optarg);
            tmp->next = ignore_unit;
            ignore_unit = tmp;
        } else if ('i' == c) {
            struct entry *tmp = malloc(sizeof(*tmp));
            tmp->name = strdup(optarg);
            tmp->next = process_unit;
            process_unit = tmp;
        } else {
            if ('-' == optopt) {
                break;
            }
            if ('p' == optopt || 'x' == optopt || 'i' == optopt || 'l' == optopt) {
                printf("Option -%c requires an argument\n", optopt);
            } else {
                printf("Unknown option %c\n", optopt);
            }
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind == argc) {
        printf("You need to specify binary to process\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    char *cur_path = malloc(PATH_MAX);
    if (!getcwd(cur_path, PATH_MAX)) {
        printf("Error getting current directory - %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (process_unit && ignore_unit) {
        printf("Black-listed units (-x param) are ignored because white list (-i param) is specified\n");
    }

    if (!acceptable_path) {
        acceptable_path = cur_path;
    } else {
        sr_string abs_path = get_abs_path(cur_path, acceptable_path);
        acceptable_path = CSTR(abs_path);
    }
    INFO("Processing sources under %s", acceptable_path);

    db_name = malloc(strlen(argv[optind]) + sizeof(".fr_heap"));
    strcpy(db_name, argv[optind]);
    db_name = basename(db_name);
    char *tail = db_name + strlen(db_name);

    /* remove old DBs, including the temp ones that may not be deleted after failed run */
    strcpy(tail, ".fr_mem");
    if (remove(db_name) != 0 && ENOENT != errno) {
        ERR("Cannot delete old DB - %s", strerror(errno));
        return EXIT_FAILURE;
    }   
    strcpy(tail, ".fr_heap");
    if (remove(db_name) != 0 && ENOENT != errno) {
        ERR("Cannot delete old DB - %s", strerror(errno));
        return EXIT_FAILURE;
    }
    strcpy(tail, ".fr");
    if (remove(db_name) != 0 && ENOENT != errno) {
        ERR("Cannot delete old DB - %s", strerror(errno));
        return EXIT_FAILURE;
    }

    if (DAB_OK != DAB_OPEN(db_name, DAB_FLAG_CREATE | DAB_FLAG_THREADS)) {
        return EXIT_FAILURE;
    }

    /* TODO Assume using WAL2 when it becomes available in SQLite */
    /* Switch to fastest possible SQLite mode - without any recovery. Any failure may lead to corrupted DB,
       but if Recorder failed data isn't usable anyway */
    if (DAB_UNEXPECTED != DAB_EXEC("PRAGMA journal_mode=OFF")) {    // PRAGMA returns data we are not interested in
        return EXIT_FAILURE;
    }
    if (DAB_OK != DAB_EXEC("PRAGMA synchronous=OFF")) {    // PRAGMA returns data we are not interested in
        return EXIT_FAILURE;
    }

    uid = getuid();
    gid = getgid();

    if (chown(db_name, uid, gid)) {
        ERR("Cannot change DB ownership: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    /* collect source file and line info */
    TIMER_START;
    if (SUCCESS != dbg_srcinfo(argv[optind])) {
        ERR("Cannot process source file and line debug info");
        return EXIT_FAILURE;
    }
    TIMER_STOP("Collection of dbg info");

    if (SUCCESS != record(&argv[optind])) {
        ERR("Program execution failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**************************************************************************
 *
 *  Function:   print_usage
 *
 *  Params:     name - executable name
 *
 *  Return:
 *
 *  Descr:      Print usage to stdout
 *
 **************************************************************************/
void print_usage(char *name) {
    printf("Usage: %s [-l <logfile>] [-p <path>] [-i <unit>] [-x <unit>] -- <program with params>\n",
            name);
    printf("\t-l <logfile>  - the name of log file, by default stderr\n"
           "\t-p <path>     - specifies the acceptable initial part of path for the\n\t\t\t"
                             "units composing the binary. Units located elsewhere will\n\t\t\t"
                             "be ignored. By default - current directory.\n"
           "\t-i <unit      - name of the compilation unit to include, may occur\n\t\t\tseveral times.\n"
           "\t-x <unit>     - name of the compilation unit to exclude, may occur\n\t\t\tseveral times.\n\t\t\t"
                             "All -x params are ignored if any number of -i params are specified.\n");
};


/**************************************************************************
 *
 *  Function:   get_abs_path
 *
 *  Params:     curdir - path to current directory
 *              path - relative or absolute path
 *
 *  Return:     absolute path for the 'path' argument
 *
 *  Descr:      If 'path' is relative, return absolute path, otherwise just return 'path'
 *
 **************************************************************************/
sr_string get_abs_path(char *curdir, char *path) {

    sr_string abspath, dirname = NULL;
    char    *tmpptr, *slash;
    sr_string ret;

    if ('/' == path[0]) {
        abspath = sr_new(path, 0);  // path is absolute path
    } else {
        abspath = sr_new(curdir, 80);
        dirname = sr_new("", 80);
        do {
            tmpptr = strchr(path, '/');
            if (tmpptr) {
                STRNCPY(dirname, path, tmpptr-path);
                CSTR(dirname)[tmpptr - path] = 0;
                path = tmpptr+1;    // advance path to next item
            } else {
                STRCPY(dirname, path);
            }
            if (!STRCMP(dirname, ".")) {   // "./" directory - just skip
                continue;
            } else if (!STRCMP(dirname, "..")) {    // "../" directory
                /* cut last directory from current path */
                slash = strrchr(CSTR(abspath), '/');
                if (!slash) {
                    ERR("Cannot build absolute path for %s (current dir - %s)", path, curdir);
                    RETCLEAN(NULL);
                }
                *slash = 0;
                abspath->len = strlen(CSTR(abspath));
            } else {
                CONCAT(abspath, (char)'/', dirname);
            }
        } while (tmpptr);
    }
    ret = abspath;

cleanup:
    if (dirname) {
        STRFREE(dirname);
    }
    return ret;
}

