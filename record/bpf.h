/**************************************************************************
 *
 *  File:       bpf.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      eBPF program tracing
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
#ifndef _BPF_H
#define _BPF_H

#include <stdint.h>

#define BPF_EVT_PAGEFAULT   1
#define BPF_EVT_MMAPENTRY   2
#define BPF_EVT_MMAPEXIT    3
#define BPF_EVT_MUNMAP      4
#define BPF_EVT_BRK         5
#define BPF_EVT_SIGNAL      6

struct bpf_event {
    uint64_t    type;
    uint64_t    payload;
};

int bpf_start(pid_t pid, void (* callback)(void *, void *, int));
void bpf_stop(void);

#endif
