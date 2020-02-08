/**************************************************************************
 *
 *  File:       sr_internal.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Managed strings library internal include
 *
 *  Notes:      These functions, types and macros should not be used
 *              directly by application code
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
#ifndef _STINGRAY_INTERNAL_H
#define _STINGRAY_INTERNAL_H

#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>

struct sr {
    char        *val;   // c-string
    size_t      size;   // allocated size
    size_t      len;    // actual length of val (not incl. \0)
};

static inline char *sr_string_val(int dummy, ...) {
    va_list ap;
    va_start(ap, dummy);
    struct sr *tmp =(struct sr *)va_arg(ap, struct sr *);
    va_end(ap);
    return tmp->val;
}

#define STR_NATIVE  1
#define STR_SR      2

#define S_TYPE(A) _Generic((A), \
        char *:                 STR_NATIVE, \
        unsigned char *:        STR_NATIVE, \
        const char *:           STR_NATIVE, \
        const unsigned char *:  STR_NATIVE, \
        struct sr *:            STR_SR), \
    (A)

struct sr *sr_copy(struct sr *target, int type, const void* source, size_t max);
struct sr *sr_cat(struct sr *target, int type, const void* source, size_t max);
struct sr *sr_concat(struct sr *target, ...);

static inline int sr_string_len(int type, const void *str) {
    return STR_NATIVE == type ? strlen((char *)str) : ((struct sr *)str)->len;
}
/* I have to declare sr_new also here, in order to call it from sr_strdup */
struct sr *sr_new(const char *initval, size_t initsize);
static inline struct sr* sr_strdup(int type, const void *str) {
    return sr_new(STR_NATIVE == type ? (char *)str : ((struct sr *)str)->val, 0);
}
#define F_ANY_ANY(A, B) static inline B sr_ ## A \
                            (int t1, const void *a, int t2, const void* b) { \
    return A(STR_NATIVE == t1 ? (char *)a : ((struct sr *)a)->val, \
             STR_NATIVE == t2 ? (char *)b : ((struct sr *)b)->val); }
#define F_ANY_INT(A, B) static inline B sr_ ## A \
                                        (int t1, const void *a, int b) { \
    return A(STR_NATIVE == t1 ? (char *)a : ((struct sr *)a)->val, b); }

F_ANY_ANY(strstr, char*)
F_ANY_ANY(strcmp, int)
F_ANY_INT(strchr, char*)
F_ANY_INT(strrchr, char*)

#define _CSTR(A) (A)->val
#define _SETLEN(A,B) (A)->len = (B)
#define _STRCLEAR(A) do { \
        (A)->val[0] = 0; \
        (A)->len = 0; \
    } while (0)
#define _STRISEMPTY(A)  !((A)->len)
#define _SPRINTF(A, ...) do { \
        size_t bufsz = snprintf(NULL, 0, __VA_ARGS__); \
        sr_ensure_size(A, bufsz+1, 0); \
        (A)->len = sprintf((A)->val, __VA_ARGS__); \
    } while (0)
#define _RTRIM(A) do { \
        while (isspace((A)->val[(A)->len-1])) ((A)->len)--; \
        (A)->val[(A)->len] = 0; \
    } while (0)
#define _STRFREE(A)  do {\
        if (A) { \
            if ((A)->val) \
                free((A)->val);   \
            free(A); \
            A = NULL; \
        } \
    } while (0)

#endif

