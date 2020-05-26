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
CFLAGS := -DTIMING -D_GNU_SOURCE -Wall -Wextra -g -I../stingray -I../dab -I..
LDFLAGS := -pthread

# libbpf package adds either libbpf or libbcc library
ifeq (,$(findstring libbpf,$(shell /sbin/ldconfig -p)))
    LIBBPF = -lbcc
else
    LIBBPF = -lbpf
endif
LDLIBS := -lsqlite3 -ldwarf -lelf $(LIBBPF)

# libbpf.h may be missing declaration of perf_reader_cb type but bpf.c needs it, so
# add a macro for it and for some changed function names
BPF_FLAGS := $(shell printf "\#include <stddef.h>\n\#include <stdint.h>\n\#include <fcntl.h>\n\#include <bcc/libbpf.h>\nperf_reader_cb foo;" \
			| gcc -c -xc - 2>/dev/null || echo "-imacros bpf_compat.h -Wno-unused-parameter")

.PHONY : all clean depend install uninstall

DEPEND = ../dab/dab.o ../stingray/stingray.o
OBJFILES = record.o db.o run.o dbginfo.o memdiff.o channel.o db_workers.o \
	memcache.o bpf.o reset_dirty.o

all: fr_record fr_preload.so

fr_record: $(OBJFILES) $(DEPEND)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

fr_preload.so: preload.c
	$(CC) $(CFLAGS) -ldl -shared -fPIC $^ -o $@

memdiff.o: memdiff.c
# explicitly allow support for AVX512. Actual decision re using AVX512 or AVX2 or SSE2 is made at runtime
	$(CC) $(CFLAGS) -march=native -c -o $@ memdiff.c

bpf.o: bpf.c
	$(CC) $(CFLAGS) $(BPF_FLAGS) -c -o $@ bpf.c

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
run.o: channel.h bpf.h db_workers.h reset_dirty.h
dbginfo.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
dbginfo.o: ../dab/dab.h ../eel.h ../flightrec.h record.h
channel.o: ../eel.h channel.h
db_workers.o: ../stingray/stingray.h ../generics.h ../stingray/sr_internal.h
db_workers.o: ../dab/dab.h ../flightrec.h channel.h db_workers.h ../mem.h
db_workers.o: ../eel.h
memcache.o: ../flightrec.h record.h ../stingray/stingray.h ../generics.h
memcache.o: ../stingray/sr_internal.h ../eel.h ../mem.h memcache.h
memcache.o: db_workers.h channel.h
bpf.o: ../flightrec.h ../eel.h bpf.h
reset_dirty.o: ../flightrec.h ../eel.h reset_dirty.h
