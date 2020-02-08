/**************************************************************************
 *
 *  File:       eel.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Error handling and logging
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
#ifndef _EEL_H
#define _EEL_H

#include <stdio.h>
#include <string.h>

/* use of RETCLEAN() requires local ret variable, and cleanup label */
#define RETCLEAN(A) do { ret = A; goto cleanup; } while(0)

extern FILE *logfd;

/* POSIX basename() may modify the perameter, therefore it cannot be called
 * for string literal, so it is safer to roll my own */
#define LOCAL_LOG(T, F, L, ...) do { \
        if (!logfd) break; \
        char *tmp = strrchr((F), '/'); \
        fprintf(logfd, "%c:%s:%d:", (T), tmp ? tmp + 1 : (F), (L)); \
        fprintf(logfd, __VA_ARGS__); \
        fprintf(logfd, "\n"); \
        fflush(logfd); \
    } while(0)

#define LOG(T, ...) LOCAL_LOG(T, __FILE__, __LINE__, __VA_ARGS__)

#define ERR(...)    LOG('E', __VA_ARGS__)
#define WARN(...)   LOG('W', __VA_ARGS__)
#define INFO(...)   LOG('I', __VA_ARGS__)
/* DEBUG messages are only printed if DEBUG defined */
#ifdef DEBUG
#define DBG(...)    LOG('D', __VA_ARGS__)
#else
#define DBG(...)     
#endif

#define STR(a) XSTR(a)
#define XSTR(a) #a

#endif

