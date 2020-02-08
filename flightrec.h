/**************************************************************************
 *
 *  File:       flightrec.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Common definitions
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
#ifndef _FLIGHTREC_H
#define _FLIGHTREC_H

#define SUCCESS         0
#define FAILURE         1
#define END             2
#define MALFUNCTION     3
#define FOUND           4

/* kind of variable */
#define VKIND_VAR       1
#define VKIND_MEMBER    2
#define VKIND_PARAM     3
#define VKIND_ENUM      4

/* variable scope */
#define SCOPE_UNSET     0
#define SCOPE_GLOBAL    1
#define SCOPE_FILE      2
#define SCOPE_FUN       3
#define SCOPE_BLOCK     4
#define SCOPE_NA        10

/* type kind - first 8 bits set basic type, rest are bitmap specifying type attributes */
#define TKIND_SIGNED    1
#define TKIND_UNSIGNED  2
#define TKIND_FLOAT     3
#define TKIND_POINTER   4
#define TKIND_STRUCT    5
#define TKIND_UNION     6
#define TKIND_ARRAY     7
#define TKIND_FUNC      8
#define TKIND_CLASS     9
#define TKIND_ENUM      10
#define TKIND_TYPE      0x0000FFFF

#define TKIND_CONST     0x00010000
#define TKIND_VOLATILE  0x00020000
#define TKIND_RESTRICT  0x00040000
#define TKIND_ALIAS     0x00080000
#define TKIND_ATTRS     0xFFFF0000

/* aggregate member kind */
#define MKIND_DATA      1
#define MKIND_METHOD    2
#define MKIND_ENUM      3

/* symbol type */
#define SYMTYPE_UNKNOWN 0
#define SYMTYPE_VAR     1
#define SYMTYPE_LABEL   2
#define SYMTYPE_FUNC    3

#define GLOBAL_SCOPE    0

#ifndef ULONG
#define ULONG unsigned long
#endif

#ifndef ULLONG
#define ULLONG unsigned long long
#endif

#ifndef LLONG
#define LLONG long long
#endif

#ifdef __x86_64__
#define REG_TYPE unsigned long long
#else
/* sys/user.h define 32bit registers as signed */
#define REG_TYPE long
#endif

#endif

