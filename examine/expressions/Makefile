##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Expression parser/evaluator makefile
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
CFLAGS := -Wall -Wextra -g3 -I../../dab -I../../stingray -I../.. -I../../jsonapi -I..

.PHONY: all clean

all: libexpr.a

libexpr.a: expr.tab.o lex.yy.o expression.o
	ar -rcs $@ $^

test: test.c expression.h libexpr.a
	$(CC) -o $@ test.c -lfl -lexpr -L.

expression.o: expression.c expression.h
	$(CC) -c $(CFLAGS) -o $@ expression.c

lex.yy.o: expr.l expr.tab.h expression.h
	flex expr.l
	$(CC) -c -o $@ lex.yy.c -I../.. -I../../jsonapi

expr.tab.o: expr.y expression.h
	bison -d expr.y
#	bison -d -t expr.y
	$(CC) -c -o $@ expr.tab.c -I../.. -I../../jsonapi

clean:
	rm -f lex.yy.c expr.tab.c expr.tab.h test expression.o lex.yy.o expr.tab.o libexpr.a

