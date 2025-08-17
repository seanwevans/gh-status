CC ?= gcc
CFLAGS ?= -Wall -Wextra
LDFLAGS ?= -lncursesw

.PHONY: all clean test

all: ghstatus

ghstatus: ghstatus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: test_status
	./test_status

test_status: test_status.c ghstatus.c
	$(CC) $(CFLAGS) -o $@ test_status.c $(LDFLAGS)

clean:
	rm -f ghstatus test_status
