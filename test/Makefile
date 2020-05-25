##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Tester, tast programs and automated tests
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
CFLAGS := -g3 -gdwarf-2 -O0

.PHONY : all run

##### test binaries

TESTBINS = test1 test2 test3 test4 test5

all: $(TESTBINS) tester

run: tester
run: ${TESTBINS:%=%.fr}
	./tester -v path="$(shell pwd)" *.test

%.fr: %
	fr_record -l test.log -- ./$^

.SECONDEXPANSION:
$(TESTBINS): $$(addsuffix *.c,$$@)
	$(CC) $(CFLAGS) -o $@ $^

##### tester

tester: y.tab.o lex.yy.o tester_func.o ../jsonapi/jsonapi.o
	$(CC) -o $@ $^ -lfl -ljson-c -lm

lex.yy.o: tester.l y.tab.h tester.h
	lex tester.l
	$(CC) -I../jsonapi -g -c -o $@ lex.yy.c

y.tab.o: tester.y tester.h Makefile
	yacc -d tester.y
#	yacc -d -t tester.y
	$(CC) -I../jsonapi -g -c -o $@ y.tab.c

tester_func.o: tester_func.c tester.h
	$(CC) -I../jsonapi -g -c -o $@ tester_func.c -I..

clean:
	rm -f $(TESTBINS) *.o *.fr* test.log tester lex.yy.c y.tab.c y.tab.h core.*

