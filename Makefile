CC ?= gcc
CFLAGS ?= -Wall -Wextra
LDFLAGS ?= -lncursesw

.PHONY: all clean test

all: ghstatus

ghstatus: ghstatus.c status.o
	$(CC) $(CFLAGS) -o $@ ghstatus.c status.o $(LDFLAGS)

status.o: status.asm
	nasm -f elf64 $< -o $@

test: test_status
	./test_status

test_status: test_status.c status.o
	$(CC) $(CFLAGS) -o $@ test_status.c status.o $(LDFLAGS)

clean:
	rm -f ghstatus test_status status.o
