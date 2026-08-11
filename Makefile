CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -lm
PREFIX ?= /usr/local

.PHONY: all clean install test

all: archetypon

archetypon: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) main.c $(LDLIBS) -o "$@"

install: archetypon
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 archetypon "$(DESTDIR)$(PREFIX)/bin/archetypon"

test: archetypon
	./tests/test.sh

clean:
	rm -f archetypon
