CC ?= gcc
CFLAGS ?= -Wall -Wextra
LDFLAGS ?= -lncursesw

.PHONY: all clean test

all: ghstatus

ghstatus: ghstatus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

ghstatus-hs: ghstatus.hs
	ghc -O2 -o $@ $<

test: test_status
	./test_status

test_status: test_status.c ghstatus.c
	$(CC) $(CFLAGS) -o $@ test_status.c $(LDFLAGS)

clean:
	rm -f ghstatus test_status ghstatus-hs
