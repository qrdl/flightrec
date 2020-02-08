/**************************************************************************
 *
 *  File:       stingray.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Managed strings library public include
 *
 *  Notes:      Application code must use only macros and functions from
 *              this header
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
#ifndef _STINGRAY_H
#define _STINGRAY_H

#include "generics.h"
#include "sr_internal.h"

typedef struct sr *sr_string;

/* Standard C string API replacement functions */
#define STRCPY(A,B) sr_copy((A), S_TYPE(B), SIZE_MAX)
#define STRNCPY(A,B,N) sr_copy((A), S_TYPE(B), (N))
#define STRCAT(A,B) sr_cat((A), S_TYPE(B), SIZE_MAX)
#define STRNCAT(A,B,N) sr_cat((A), S_TYPE(B), (N))
#define STRLEN(A) sr_string_len(S_TYPE(A))
#define STRSTR(A,B) sr_strstr(S_TYPE(A), S_TYPE(B))
#define STRCMP(A,B) sr_strcmp(S_TYPE(A), S_TYPE(B))
#define STRCHR(A,B) sr_strchr(S_TYPE(A), (B))
#define STRRCHR(A,B) sr_strrchr(S_TYPE(A), (B))
#define STRDUP(A) sr_strdup(S_TYPE(A))

/* Stingray additional functions */
#define CONCAT(A, ...) sr_concat((A), VAR(NUMARGS(__VA_ARGS__), ##__VA_ARGS__))
#define CSTR(A) _CSTR(A)
#define STRCLEAR(A) _STRCLEAR(A)
#define STRISEMPTY(A)  _STRISEMPTY(A)
#define SPRINTF(A, ...) _SPRINTF(A, __VA_ARGC__)
#define RTRIM(A) _RTRIM(A)
#define STRFREE(A) _STRFREE(A)
#define SETLEN(A,B) _SETLEN((A),(B))

sr_string sr_new(const char *initval, size_t initsize);
void sr_ensure_size(sr_string str, size_t minsize, size_t extend);

#endif

