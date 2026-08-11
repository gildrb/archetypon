CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -lm
PREFIX ?= /usr/local

.PHONY: all clean install test

all: diopton

diopton: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) main.c $(LDLIBS) -o "$@"

install: diopton
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 diopton "$(DESTDIR)$(PREFIX)/bin/diopton"

test: diopton
	./tests/test.sh

clean:
	rm -f diopton
