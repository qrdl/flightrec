/**************************************************************************
 *
 *  File:       memdiff.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Memory comparison
 *
 *  Notes:      Use most effective available CPU instructions for comparing
 *              memory buffers. Intrinsics are taken from
 *              https://software.intel.com/sites/landingpage/IntrinsicsGuide
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
#include <x86intrin.h>
#include <stdint.h>

static int memdiff64(const char *buf1, const char *buf2, size_t size);
static int memdiff32(const char *buf1, const char *buf2, size_t size);
static int memdiff16(const char *buf1, const char *buf2, size_t size);
static int memdiff8(const char *buf1, const char *buf2, size_t size);

/**************************************************************************
 *
 *  Function:   best_memdiff
 *
 *  Params:     size
 *
 *  Return:     pointer to memory comparison function
 *
 *  Descr:      Finds most effective implementation of memory comparison
 *              for the CPU and given buffer size
 *
 **************************************************************************/
int (* best_memdiff(size_t size))(const char *, const char *, size_t) {
    /* test for AVX512DQ rather then AVX512F because testing function _kortestz_mask8_u8() requires AVX512DQ,
       and AVX512DQ implies AVX512F */
#ifdef __AVX512DQ__
    if (size >= 64 && __builtin_cpu_supports("avx512dq")) {    // 64-byte vectors supported
        return &memdiff64;
    }
#endif
#ifdef __AVX2__
    if (size >= 32 && __builtin_cpu_supports("avx2")) {        // 32-byte vectors supported
        return &memdiff32;
    }
#endif
#ifdef __SSE2__
    if (size >= 16 && __builtin_cpu_supports("sse2")) {        // 16-byte vectors supported
        return &memdiff16;
    }
#endif

    return  &memdiff8;        // last resort - use 8-byte comparison only
}


#define STEP(A) do { \
        size -= (A); \
        buf1 += (A); \
        buf2 += (A); \
} while(0)

#ifdef __AVX512DQ__
/**************************************************************************
 *
 *  Function:   memdiff64
 *
 *  Params:     buf1 - first buffer (must be aligned to 64-byte boundary)
 *              buf2 - second biffer (must be aligned to 64-byte boundary)
 *              size - size of buffers
 *
 *  Return:     1 (buffers differ) / 0 (buffers match)
 *
 *  Descr:      Compare memory using AVX512 CPU instructions
 *
 *  Notes:      FIXME: Not tested as I have no access to Xeon CPUs
 *
 **************************************************************************/
int memdiff64(const char *buf1, const char *buf2, size_t size) {
    while (size >= 64) {
        if (!_kortestz_mask8_u8(0, _mm512_cmpeq_epi64_mask(_mm512_load_epi64(buf1), _mm512_load_epi64(buf2)))) {
            return 1;
        }
        STEP(64);
    }

    if (size > 0) {
        return memdiff32(buf1, buf2, size);
    }

    return 0;
}
#endif


#ifdef __AVX2__
/**************************************************************************
 *
 *  Function:   memdiff32
 *
 *  Params:     buf1 - first buffer (must be aligned to 32-byte boundary)
 *              buf2 - second biffer (must be aligned to 32-byte boundary)
 *              size - size of buffers
 *
 *  Return:     1 (buffers differ) / 0 (buffers match)
 *
 *  Descr:      Compare memory using AVX2 CPU instructions
 *
 **************************************************************************/
int memdiff32(const char *buf1, const char *buf2, size_t size) {
    while (size >= 32) {
        if ((int)0xFFFFFFFF != _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_load_si256((__m256i const *)buf1), _mm256_load_si256((__m256i const *)buf2)))) {
            return 1;
        }
        STEP(32);
    }

    if (size > 0) {
        return memdiff16(buf1, buf2, size);
    }

    return 0;
}
#endif


#ifdef __SSE2__
/**************************************************************************
 *
 *  Function:   memdiff16
 *
 *  Params:     buf1 - first buffer (must be aligned to 16-byte boundary)
 *              buf2 - second biffer (must be aligned to 16-byte boundary)
 *              size - size of buffers
 *
 *  Return:     1 (buffers differ) / 0 (buffers match)
 *
 *  Descr:      Compare memory using SSE2 CPU instructions
 *
 **************************************************************************/
int memdiff16(const char *buf1, const char *buf2, size_t size) {
    while (size >= 16) {
        if (0xFFFF != _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((__m128i const *)buf1), _mm_load_si128((__m128i const *)buf2)))) {
            return 1;
        }
        STEP(16);
    }

    if (size > 0) {
        return memdiff8(buf1, buf2, size);
    }

    return 0;
}
#endif


#define BYTE(B,P) (*((uint8_t *)(B) + (P)))
/**************************************************************************
 *
 *  Function:   memdiff8
 *
 *  Params:     buf1 - first buffer
 *              buf2 - second biffer
 *              size - size of buffers
 *
 *  Return:     1 (buffers differ) / 0 (buffers match)
 *
 *  Descr:      Compare memory using 64-bit integer comparison
 *
 **************************************************************************/
int memdiff8(const char *buf1, const char *buf2, size_t size) {
    uint64_t one, two;
    size_t cur;
    size_t limit = size / sizeof(uint64_t);
    for (cur = 0; cur < limit; cur++) {
        one = *((uint64_t *)buf1 + cur);
        two = *((uint64_t *)buf2 + cur);
        if (one != two) {
            return 1;
        }
    }

    // check remaining bytes, if any
    if (limit * sizeof(uint64_t) < size) {
        for (size_t pos = cur * 8; pos < size; pos++ ) {
            if (BYTE(buf1, pos) != BYTE(buf2, pos))
                return 1;
        }
    }

    return 0;       // buffers are identical
}

