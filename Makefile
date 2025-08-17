CC ?= gcc
CFLAGS ?= -Wall -Wextra
LDFLAGS ?= -lncursesw

.PHONY: all clean

all: ghstatus

ghstatus: ghstatus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f ghstatus
