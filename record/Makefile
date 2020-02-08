##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Record makefile
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
CFLAGS := -D_GNU_SOURCE -Wall -Wextra -g -I../stingray -I../dab -I..
LDLIBS := -lsqlite3 -ldwarf -lelf
LDFLAGS := -pthread

.PHONY : all clean depend

DEPEND = ../dab/dab.o ../stingray/stingray.o
OBJFILES = record.o db.o run.o dbginfo.o memdiff.o mem.o channel.o workers.o

all: fr_record fr_preload.so

fr_record: $(OBJFILES) $(DEPEND)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)
	cp $@ ..

fr_preload.so: preload.c
	$(CC) $(CFLAGS) -ldl -shared -fPIC $^ -o $@
	cp $@ ..

memdiff.o: memdiff.c
# explicitly allow support for AVX512. Actual decision re using AVX512 or AVX2 or SSE2 is made at runtime
	$(CC) $(CFLAGS) -c -mavx512dq memdiff.c -o $@

mem.o: mem.c
# explicitly allow support for AVX. Actual decision re using AVX is made at runtime
	$(CC) $(CFLAGS) -c -mavx mem.c -o $@

clean:
	rm -f fr_record fr_preload.so $(OBJFILES) core

depend: $(OBJFILES:.o=.c)
	makedepend -Y. -I../stingray -I../dab -I.. -I. $^ 2>/dev/null
# DO NOT DELETE

record.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
record.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
db.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
db.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
run.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
run.o: ../eel.h ../dab/dab.h ../flightrec.h record.h ../mem.h channel.h
run.o: workers.h
dbginfo.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
dbginfo.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
mem.o: ../eel.h ../stingray/stingray.h ../generics.h
mem.o: ../stingray/sr_internal.h ../dab/dab.h ../flightrec.h record.h
mem.o: ../mem.h channel.h workers.h
channel.o: ../eel.h channel.h
workers.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
workers.o: ../dab/dab.h ../flightrec.h channel.h workers.h ../mem.h ../eel.h
