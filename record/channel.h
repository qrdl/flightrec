/**************************************************************************
 *
 *  File:       channel.h
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Inter-thread channels
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
#ifndef _CHANNEL_H
#define _CHANNEL_H

#include <stddef.h>

#define CHANNEL_OK      1
#define CHANNEL_FAIL    2
#define CHANNEL_MISREAD 3
#define CHANNEL_END     4

struct chanel;

struct channel *ch_create(void);
int ch_write(struct channel *ch, char *buf, size_t bufsize);
int ch_read(struct channel *ch, char **buf, size_t *bufsize);
int ch_finish(struct channel *ch);
void ch_destroy(struct channel *ch);

#endif
