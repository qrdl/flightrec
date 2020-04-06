CFLAGS := -g3 -gdwarf-2 -O0

.PHONY : all prepare run

OBJFILES = a.o b.o

all: test tester

test: $(OBJFILES)
	$(CC) $(CFLAGS) $^ -o $@

test.fr prepare: test
#	dwarfdump ./test >test.dump
	../fr_record -l test.log -- ./test

run: test.fr tester
	./tester -v path="$(shell pwd)" *.test

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
	rm -f test test.fr* test.dump $(OBJFILES) test.log tester lex.yy.c y.tab.c lex.yy.o y.tab.o y.tab.h tester_func.o

