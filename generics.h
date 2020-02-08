/**************************************************************************
 *
 *  File:       generics.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Generics
 *
 *  Notes:      Can be used for creating functions which take different
 *              types of parameters
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
#ifndef _GENERICS_H
#define _GENERICS_H

#include <stdint.h>
#ifndef ULONG
#define ULONG uint64_t
#endif

/*
 * Macros for processing variadic calls, with params of different types *
 */
#define GEN_DATATYPE_INT     1
#define GEN_DATATYPE_UINT    2
#define GEN_DATATYPE_SHRT    3
#define GEN_DATATYPE_USHRT   4
#define GEN_DATATYPE_LNG     5
#define GEN_DATATYPE_ULNG    6
#define GEN_DATATYPE_LLNG    7
#define GEN_DATATYPE_ULLNG   8
#define GEN_DATATYPE_CHR     9
#define GEN_DATATYPE_UCHR    10
#define GEN_DATATYPE_STR     11
#define GEN_DATATYPE_USTR    12
#define GEN_DATATYPE_FLT     13
#define GEN_DATATYPE_DBL     14
#define GEN_DATATYPE_SR      15

/* ##__VA_ARGS__ is GCC extension, also supported by Clang */
#define NUMARGS(...) NUMARGS__(0, ##__VA_ARGS__, 15, 14, 13, 12, 11, 10, 9, 8, \
       7, 6, 5, 4, 3, 2, 1, 0)
#define NUMARGS__(Z, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, \
        _14, _15, N, ...) N

/* generic param processing for non-pointers */
#define V(A) _Generic((A), \
        int:                    GEN_DATATYPE_INT, \
        unsigned int:           GEN_DATATYPE_UINT, \
        short:                  GEN_DATATYPE_SHRT, \
        unsigned short:         GEN_DATATYPE_USHRT, \
        long:                   GEN_DATATYPE_LNG, \
        unsigned long:          GEN_DATATYPE_ULNG, \
        long long:              GEN_DATATYPE_LLNG, \
        unsigned long long:     GEN_DATATYPE_ULLNG, \
        char:                   GEN_DATATYPE_CHR, \
        unsigned char:          GEN_DATATYPE_UCHR, \
        char *:                 GEN_DATATYPE_STR, \
        unsigned char *:        GEN_DATATYPE_USTR, \
        const char *:           GEN_DATATYPE_STR, \
        const unsigned char *:  GEN_DATATYPE_USTR, \
        float:                  GEN_DATATYPE_FLT, \
        double:                 GEN_DATATYPE_DBL, \
        sr_string:              GEN_DATATYPE_SR \
), A

#define  V0(A)                                               0
#define  V1(_1)                                              V(_1),0
#define  V2(_1,_2)                                           V(_1),V(_2),0
#define  V3(_1,_2,_3)                                        V(_1),V(_2),V(_3),0
#define  V4(_1,_2,_3,_4)                                     V(_1),V(_2),V(_3),\
    V(_4),0
#define  V5(_1,_2,_3,_4,_5)                                  V(_1),V(_2),V(_3),\
    V(_4),V(_5),0
#define  V6(_1,_2,_3,_4,_5,_6)                               V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),0
#define  V7(_1,_2,_3,_4,_5,_6,_7)                            V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),0
#define  V8(_1,_2,_3,_4,_5,_6,_7,_8)                         V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),0
#define  V9(_1,_2,_3,_4,_5,_6,_7,_8,_9)                      V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),0
#define V10(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A)                   V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),V(_A),0
#define V11(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B)                V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),V(_A),V(_B),0
#define V12(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C)             V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),V(_A),V(_B),V(_C),0
#define V13(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C,_D)          V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),V(_A),V(_B),V(_C),V(_D),0
#define V14(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C,_D,_E)       V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),V(_A),V(_B),V(_C),V(_D),V(_E),0
#define V15(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C,_D,_E,_F)    V(_1),V(_2),V(_3),\
    V(_4),V(_5),V(_6),V(_7),V(_8),V(_9),V(_A),V(_B),V(_C),V(_D),V(_E),V(_F),0

#define VAR_(A) V ## A
#define VAR(A, ...) VAR_(A)(__VA_ARGS__)

/* generic param processing for pointers (when param is where to store the result) */
#define P(A) _Generic((A), \
        int *:              GEN_DATATYPE_INT, \
        unsigned int *:     GEN_DATATYPE_UINT, \
        short *:            GEN_DATATYPE_SHRT, \
        unsigned short *:   GEN_DATATYPE_USHRT, \
        long *:             GEN_DATATYPE_LNG, \
        unsigned long *:    GEN_DATATYPE_ULNG, \
        long long *:        GEN_DATATYPE_LLNG, \
        unsigned long long*:GEN_DATATYPE_ULLNG, \
        char *:             GEN_DATATYPE_CHR, \
        unsigned char *:    GEN_DATATYPE_UCHR, \
        char **:            GEN_DATATYPE_STR, \
        unsigned char **:   GEN_DATATYPE_USTR, \
        float *:            GEN_DATATYPE_FLT, \
        double *:           GEN_DATATYPE_DBL, \
        sr_string:          GEN_DATATYPE_SR \
), A

#define  P0(A)                                               0
#define  P1(_1)                                              P(_1),0
#define  P2(_1,_2)                                           P(_1),P(_2),0
#define  P3(_1,_2,_3)                                        P(_1),P(_2),P(_3),0
#define  P4(_1,_2,_3,_4)                                     P(_1),P(_2),P(_3),\
    P(_4),0
#define  P5(_1,_2,_3,_4,_5)                                  P(_1),P(_2),P(_3),\
    P(_4),P(_5),0
#define  P6(_1,_2,_3,_4,_5,_6)                               P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),0
#define  P7(_1,_2,_3,_4,_5,_6,_7)                            P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),0
#define  P8(_1,_2,_3,_4,_5,_6,_7,_8)                         P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),0
#define  P9(_1,_2,_3,_4,_5,_6,_7,_8,_9)                      P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),0
#define P10(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A)                   P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),P(_A),0
#define P11(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B)                P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),P(_A),P(_B),0
#define P12(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C)             P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),P(_A),P(_B),P(_C),0
#define P13(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C,_D)          P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),P(_A),P(_B),P(_C),P(_D),0
#define P14(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C,_D,_E)       P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),P(_A),P(_B),P(_C),P(_D),P(_E),0
#define P15(_1,_2,_3,_4,_5,_6,_7,_8,_9,_A,_B,_C,_D,_E,_F)    P(_1),P(_2),P(_3),\
    P(_4),P(_5),P(_6),P(_7),P(_8),P(_9),P(_A),P(_B),P(_C),P(_D),P(_E),P(_F),0

#define PVAR_(A) P ## A
#define PVAR(A, ...) PVAR_(A)(__VA_ARGS__)

#endif

