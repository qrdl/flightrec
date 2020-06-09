##########################################################################
#
#  File:       	Makefile
#
#  Project:    	Flight recorder (https://github.com/qrdl/flightrec)
#
#  Descr:      	Test programs and test suite
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

.PHONY : all run clean

TESTBINS = test01 test02 test03 test04 test05 test06 test07 test08 test09 test10

all: $(TESTBINS)

run: ${TESTBINS:%=%.fr}
	../tester/tester -v path="$(shell pwd)" *.test

%.fr: %
	fr_record -l test.log -- ./$^

.SECONDEXPANSION:
$(TESTBINS): $$(addsuffix *.c,$$@)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TESTBINS) *.o *.fr* test.log core.*

