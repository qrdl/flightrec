##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Tester for test suite
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
.PHONY : all clean

all: tester

tester: tester.tab.o lex.yy.o tester_func.o ../jsonapi/jsonapi.o
	$(CC) -o $@ $^ -lfl -ljson-c -lm

lex.yy.o: tester.l tester.tab.h tester.h
	flex tester.l
	$(CC) -I../jsonapi -g -c -o $@ lex.yy.c

tester.tab.o: tester.y tester.h Makefile
	bison -d tester.y
#	bison -d -t tester.y
	$(CC) -I../jsonapi -g -c -o $@ tester.tab.c

tester_func.o: tester_func.c tester.h
	$(CC) -I../jsonapi -g -c -o $@ tester_func.c -I..

clean:
	rm -f *.o *.fr* tester lex.yy.c tester.tab.c tester.tab.h

