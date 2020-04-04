##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Examine makefile
#
#  Notes:
#
##########################################################################
#
#  Copyright (C) 2017-2020 Ilya Caramishev (ilya@qrdl.com)
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
CFLAGS := -Wall -Wextra -Wno-format-security -g3 -I../dab -I../stingray -I../jsonapi -I..
LDLIBS := -lsqlite3 -ldwarf -lelf -ljson-c -lexpr -Lexpressions
LDFLAGS := -pthread

.PHONY: all clean depend expressions/libexpr.a

DEPEND = ../dab/dab.o ../stingray/stingray.o ../jsonapi/jsonapi.o
OBJFILES = examine.o requests.o cmd_hash.o comms.o vars.o

all: fr_examine

fr_examine: $(OBJFILES) $(DEPEND) expressions/libexpr.a
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)
	cp $@ ../vscode_extension

expressions/libexpr.a:
	$(MAKE) -C expressions

cmd_hash.c: requests.h commands.gperf
	gperf -L ANSI-C -CGD -N identify -K name -t commands.gperf > $@

clean:
	rm -f fr_examine $(OBJFILES) cmd_hash.c

depend: $(OBJFILES:.o=.c)
	makedepend -Y. -I../dab -I.. -I. $^ 2>/dev/null
# DO NOT DELETE

examine.o: ../dab/dab.h ../generics.h ../eel.h examine.h ../flightrec.h
examine.o: requests.h
requests.o: ../eel.h ../dab/dab.h ../generics.h examine.h ../flightrec.h
requests.o: requests.h expressions/expression.h
cmd_hash.o: requests.h
comms.o: ../eel.h examine.h ../flightrec.h
vars.o: ../eel.h ../dab/dab.h ../generics.h ../flightrec.h examine.h ../mem.h
