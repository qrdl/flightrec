CFLAGS := -D_GNU_SOURCE -DDEBUG -Wall -Wextra -g3 -gdwarf-2 -O0

.PHONY : all run

OBJFILES = a.o b.o

all: test

test: $(OBJFILES)
	$(CC)  $^ -o $@

run: test
	dwarfdump ./test >test.dump
	../fr_record -l test.log -- ./test

clean:
	rm -f test test.fr test.dump $(OBJFILES) test.log

