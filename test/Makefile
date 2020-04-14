CFLAGS := -g3 -gdwarf-2 -O0

.PHONY : all run

##### test binaries

TESTBINS = test1 test2

all: $(TESTBINS) tester

run: tester
run: ${TESTBINS:%=%.fr}
	./tester -v path="$(shell pwd)" *.test

%.fr: %
	../fr_record -l test.log -- ./$^

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
	rm -f $(TESTBINS) *.o *.fr* test.log tester lex.yy.c y.tab.c y.tab.h

