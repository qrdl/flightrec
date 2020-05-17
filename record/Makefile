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
CFLAGS := -DTIMING -D_GNU_SOURCE -Wall -Wextra -g -I../stingray -I../dab -I..
LDFLAGS := -pthread

# libbpf package adds either libbpf or libbcc library
ifeq (,$(findstring libbpf,$(shell /sbin/ldconfig -p)))
    LIBBPF = -lbcc
else
    LIBBPF = -lbpf
endif
LDLIBS := -lsqlite3 -ldwarf -lelf $(LIBBPF)


.PHONY : all clean depend install uninstall

DEPEND = ../dab/dab.o ../stingray/stingray.o
OBJFILES = record.o db.o run.o dbginfo.o memdiff.o mem.o channel.o workers.o \
	memcache.o bpf.o

all: fr_record fr_preload.so

fr_record: $(OBJFILES) $(DEPEND)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

fr_preload.so: preload.c
	$(CC) $(CFLAGS) -ldl -shared -fPIC $^ -o $@

memdiff.o: memdiff.c
# explicitly allow support for AVX512. Actual decision re using AVX512 or AVX2 or SSE2 is made at runtime
	$(CC) $(CFLAGS) -march=native -c memdiff.c -o $@

clean:
	rm -f fr_record fr_preload.so $(OBJFILES) core

install:
ifneq "$(shell whoami)" "root"
	$(error "Must be root to run install")
else
	cp fr_record fr_preload.so /usr/bin
	chown root /usr/bin/fr_record /usr/bin/fr_preload.so
	chmod a+s /usr/bin/fr_record
endif

uninstall:
ifneq "$(shell whoami)" "root"
	$(error "Must be root to run uninstall")
else
	rm /usr/bin/fr_record /usr/bin/fr_preload.so
endif

depend: $(OBJFILES:.o=.c)
	makedepend -Y. -I../stingray -I../dab -I.. -I. $^ 2>/dev/null
# DO NOT DELETE

record.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
record.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
db.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
db.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
run.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
run.o: ../eel.h ../dab/dab.h ../flightrec.h record.h ../mem.h memcache.h
run.o: channel.h workers.h bpf.h
dbginfo.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
dbginfo.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
mem.o: ../eel.h ../stingray/stingray.h ../generics.h
mem.o: ../stingray/sr_internal.h ../dab/dab.h ../flightrec.h record.h
mem.o: ../mem.h channel.h workers.h
channel.o: ../eel.h channel.h
workers.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
workers.o: ../dab/dab.h ../flightrec.h channel.h workers.h ../mem.h ../eel.h
memcache.o: ../flightrec.h ../eel.h ../mem.h memcache.h workers.h
memcache.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
memcache.o: channel.h
bpf.o: ../flightrec.h ../eel.h bpf.h
