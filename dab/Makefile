##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Dab makefile
#
#  Notes:
#
##########################################################################
#
#  Copyright (C) 2017-2020 Ilya Caramishev (flightrec@qrdl.com)
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Affero General Public License as
#  published by the Free Software Foundation, either version 3 of the
#  License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
##########################################################################
CFLAGS := -Wall -Wextra -g3 -I.. -I../stingray
ifndef CC
    CC := gcc
endif

.PHONY : all clean test

all: dab.o

dab.o: dab.c dab.h Makefile
	$(CC) $(CFLAGS) -c -o $@ dab.c

test: test.c dab.o
	$(CC) $(CFLAGS) -o $@ $^ -pthread -lsqlite3

clean:
	rm -f test dab.o

