/**************************************************************************
 *
 *  File:       stingray.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Managed strings library
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <printf.h>

#include "stingray.h"

#ifndef EXTEND
#define EXTEND 64
#endif

static int print_sr(FILE *stream, const struct printf_info *info, const void *const *args);
static int print_sr_arginfo(const struct printf_info *info, size_t n, int *argtypes, int *size);
static int registered;

/**************************************************************************
 *
 *  Function:   new_sr
 *
 *  Params:     initval - initial value to be set for string
 *              initsize - size to allocate for the string
 *
 *  Return:     allocated string / NULL on error
 *
 *  Descr:      Allocates managed string of size
 *              MAX(initsize, strlen(initval)+1)
 *
 **************************************************************************/
struct sr *sr_new(const char *initval, size_t initsize) {
    struct sr *new_str;
    size_t    len;

    /* register handler for 'S' format specifier */
    if (!registered) {
        register_printf_specifier('S', print_sr, print_sr_arginfo);
        registered = 1;
    }

    len = strlen(initval);
    if (initsize <= len) {
        initsize = len+1;
    }

    new_str = malloc(sizeof(*new_str));
    if (!new_str) {
        return NULL;
    }

    if (initsize < EXTEND) {
        initsize = EXTEND;
    }
    new_str->val = (char *)malloc(initsize);
    if (!new_str->val) {
        free(new_str);
        return NULL;
    }
    new_str->val[0] = '\0';

    strcpy(new_str->val, initval);
    new_str->size = initsize;
    new_str->len = len;

    return new_str;
}


/**************************************************************************
 *
 *  Function:   sr_ensure_size
 *
 *  Params:     str - managed string
 *              minsize - minimal allowed size for the string
 *              extend - min size of string extend in case of realloc
 *
 *  Return:     N/A
 *
 *  Descr:      Check whether manager string str is big enough. If not,
 *              increase its size
 *
 **************************************************************************/
void sr_ensure_size(struct sr *str, size_t minsize, size_t extend) {

    if (str->size < minsize) {
        if (minsize < (str->size + extend)) {
            minsize = str->size + extend;
        }
        str->val = (char *)realloc(str->val, minsize);
        if (!str->val) {
            fprintf(stderr, "Fatal error - cannon grow managed string!\n");
            _exit(EXIT_FAILURE);
        }
        str->size = minsize;
    }

    return;
}


/**************************************************************************
 *
 *  Function:   sr_copy
 *
 *  Params:     dest - destination managed string
 *              type - source type (STR_NATIVE / STR_SR)
 *              source - C or Stingray source string
 *              max - max bytes of source to use (SIZE_MAX if no limit)
 *
 *  Return:     destination string
 *
 *  Descr:      Copy from the source to the destination
 *
 *  Note:       if max isn't SIZE_MAX, and the source is C string, it is not
 *              required for the source to be zero-terminated
 *              This function should not be called directly! Use STRCPY()
 *
 **************************************************************************/
struct sr *sr_copy(struct sr *dest, int type, const void* source, size_t max) {
    size_t len = SIZE_MAX;
    char *src;

    if (STR_NATIVE == type) {
        if (SIZE_MAX == max) {
            len = strlen((char*)source);
        } else {
            len = max;
        }
        src = (char *)source;
    } else {
        len = ((struct sr*)source)->len;
        if (len > max) {
            len = max;
        }
        src = ((struct sr*)source)->val;
    }

    sr_ensure_size(dest, len + 1, EXTEND);
    strncpy(dest->val, src, len);
    dest->val[len] = '\0';
    dest->len = len;

    return dest;
}


/**************************************************************************
 *
 *  Function:   sr_cat
 *
 *  Params:     dest - destination managed string
 *              type - source type (STR_NATIVE / STR_SR)
 *              source - C or Stingray source string
 *              max - max bytes of source to use (SIZE_MAX if no limit)
 *
 *  Return:     destination string
 *
 *  Descr:      Concatenate the source to the destination
 *
 *  Note:       if max isn't SIZE_MAX, and the source is C string, it is not
 *              required for the source to be zero-terminated
 *              This function should not be called directly! Use STRCAT()
 *
 **************************************************************************/
struct sr *sr_cat(struct sr *dest, int type, const void* source, size_t max) {
    size_t len = SIZE_MAX;
    char *src;

    if (STR_NATIVE == type) {
        if (SIZE_MAX == max) {
            len = strlen((char*)source);
        } else {
            len = max;
        }
        src = (char *)source;
    } else {
        len = ((struct sr *)source)->len;
        if (len > max) {
            len = max;
        }
        src = ((struct sr *)source)->val;
    }

    sr_ensure_size(dest, dest->len + len + 1, EXTEND);
    strncpy(dest->val + dest->len, src, len);
    dest->len += len;
    dest->val[dest->len] = '\0';

    return dest;
}

#define SIZE(A) sizeof(XSTR(A))
#define XSTR(A) #A

#define FMT_COMMON "%.0s%"
#define FMT_INT FMT_COMMON "d"
#define FMT_CHAR FMT_COMMON "c"
#define FMT_UINT FMT_COMMON "u"
#define FMT_LNG FMT_COMMON "ld"
#define FMT_ULNG FMT_COMMON "lu"
#define FMT_LLNG FMT_COMMON "lld"
#define FMT_ULLNG FMT_COMMON "llu"
#define FMT_STR FMT_COMMON "s"
#define FMT_SR FMT_COMMON "S"
#define FMT_DBL FMT_COMMON "lf"
/**************************************************************************
 *
 *  Function:   sr_concat
 *
 *  Params:     dest - destination managed string
 *              variable number of pairs of:
 *                  type - one of GEN_DATATYPE_XXX
 *                  source - source variable of abovementioned type
 *
 *  Return:     destination string
 *
 *  Descr:      Concatenate the sources to destination
 *
 *  Note:       This function should not be called directly! Use STRCAT()
 *              Char literals are considered ints by GCC, explicit cast
 *              to char required
 *
 **************************************************************************/
struct sr *sr_concat(struct sr *dest, ...) {
    int type, i = 0;
    size_t len = 0;
    char format[128] = {0};
    char *cursor = format;
    va_list ap;

    va_start(ap, dest);

    for (type = va_arg(ap, int); type; type = va_arg(ap, int)) {
        i++;
        switch (type) {
            case GEN_DATATYPE_INT:
                                  va_arg(ap, int);
                                  len += SIZE(INT_MAX);
                                  strcpy(cursor, FMT_INT);
                                  cursor += sizeof(FMT_INT)-1;
                                  break;
            case GEN_DATATYPE_SHRT:  // short promoted to int in variadic call
                                  va_arg(ap, int);
                                  len += SIZE(SHRT_MAX);
                                  strcpy(cursor, FMT_INT);
                                  cursor += sizeof(FMT_INT)-1;
                                  break;
            case GEN_DATATYPE_CHR:
            case GEN_DATATYPE_UCHR:  // char promoted to int in variadic call
                                  va_arg(ap, int);
                                  len += 1;
                                  strcpy(cursor, FMT_CHAR);
                                  cursor += sizeof(FMT_CHAR)-1;
                                  break;
            case GEN_DATATYPE_UINT:
                                  va_arg(ap, unsigned int);
                                  len += SIZE(UINT_MAX);
                                  strcpy(cursor, FMT_UINT);
                                  cursor += sizeof(FMT_UINT)-1;
                                  break;
            case GEN_DATATYPE_USHRT: // short promoted to int in variadic call
                                  va_arg(ap, unsigned int);
                                  len += SIZE(USHRT_MAX);
                                  strcpy(cursor, FMT_UINT);
                                  cursor += sizeof(FMT_UINT)-1;
                                  break;
            case GEN_DATATYPE_LNG:
                                  va_arg(ap, long);
                                  len += SIZE(LONG_MAX);
                                  strcpy(cursor, FMT_LNG);
                                  cursor += sizeof(FMT_LNG)-1;
                                  break;
            case GEN_DATATYPE_ULNG:
                                  va_arg(ap, unsigned long);
                                  len += SIZE(ULONG_MAX);
                                  strcpy(cursor, FMT_ULNG);
                                  cursor += sizeof(FMT_ULNG)-1;
                                  break;
            case GEN_DATATYPE_LLNG:
                                  va_arg(ap, long long);
                                  len += SIZE(LLONG_MAX);
                                  strcpy(cursor, FMT_LLNG);
                                  cursor += sizeof(FMT_LLNG)-1;
                                  break;
            case GEN_DATATYPE_ULLNG:
                                  va_arg(ap, unsigned long long);
                                  len += SIZE(ULLONG_MAX);
                                  strcpy(cursor, FMT_ULLNG);
                                  cursor += sizeof(FMT_ULLNG)-1;
                                  break;
            case GEN_DATATYPE_STR:
            case GEN_DATATYPE_USTR:
                                  len += strlen(va_arg(ap, char *));
                                  strcpy(cursor, FMT_STR);
                                  cursor += sizeof(FMT_STR)-1;
                                  break;
            case GEN_DATATYPE_SR:
                                  len += va_arg(ap, struct sr *)->len;
                                  strcpy(cursor, FMT_SR);
                                  cursor += sizeof(FMT_SR)-1;
                                  break;
            case GEN_DATATYPE_FLT: // float promoted to double in variadic call
            case GEN_DATATYPE_DBL:
                                  len += snprintf(NULL, 0, "%lf",
                                          va_arg(ap, double));
                                  strcpy(cursor, FMT_DBL);
                                  cursor += sizeof(FMT_DBL)-1;
                                  break;
        }
    }

    va_end(ap);
    /* by now we know required size and printf format - FMT_COMMON just
     * silently consumes param */

    sr_ensure_size(dest, dest->len + len + 1, EXTEND);

    va_start(ap, dest);
    dest->len += vsprintf(dest->val + dest->len, format, ap);
    va_end(ap);

    return dest;
}


/**************************************************************************
 *
 *  Function:   print_sr
 *
 *  Params:     standard for printf format specifier handler
 *
 *  Return:     number of bytes printed
 *
 *  Descr:      Print the content of SR string
 *
 *  Note:       https://www.gnu.org/software/libc/manual/html_node/Defining-the-Output-Handler.html
 *
 **************************************************************************/
int print_sr(FILE *stream, const struct printf_info *info, const void *const *args) {
    const struct sr *sr;

    sr = *((const struct sr **) (args[0]));
    if (sr->len <= 0) {
        return 0;       // print nothing
    }

    fprintf(stream, "%*s", (info->left ? -info->width : info->width), CSTR(sr));
    return sr->len;
}


/**************************************************************************
 *
 *  Function:   print_sr_arginfo
 *
 *  Params:     standard
 *
 *  Return:     1
 *
 *  Descr:      Function to obtain information about the number and type
 *              of arguments used by a conversion specifier
 *
 *  Note:       https://www.gnu.org/software/libc/manual/html_node/Defining-the-Output-Handler.html
 *
 **************************************************************************/
int print_sr_arginfo(const struct printf_info *info, size_t n, int *argtypes, int *size) {
    (void)info;     // to suppress UNUSED warning
    if (n > 0) {
        *argtypes = PA_POINTER;
        *size = sizeof(struct sr *);
    }
    return 1;
}

