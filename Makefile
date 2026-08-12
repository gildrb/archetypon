CC ?= cc
AR ?= ar
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -lm
ARFLAGS = rcs
PREFIX ?= /usr/local

LIBRARY = libarchetypon.a
LIB_SOURCES = \
	src/core.c \
	src/svg.c \
	src/image.c \
	src/png.c \
	src/webp.c \
	src/ico.c \
	src/optimize.c
LIB_OBJECTS = $(LIB_SOURCES:src/%.c=build/%.o)

.PHONY: all clean install test

all: archetypon $(LIBRARY)

archetypon: build/main.o $(LIBRARY)
	$(CC) $(LDFLAGS) build/main.o $(LIBRARY) $(LDLIBS) -o "$@"

$(LIBRARY): $(LIB_OBJECTS)
	$(AR) $(ARFLAGS) "$@" $(LIB_OBJECTS)

build/main.o: main.c archetypon.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c main.c -o "$@"

build/%.o: src/%.c src/internal.h archetypon.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

build:
	mkdir -p "$@"

build/api-test: tests/api.c archetypon.h $(LIBRARY) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/api.c $(LIBRARY) $(LDLIBS) -o "$@"

install: archetypon $(LIBRARY)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 archetypon "$(DESTDIR)$(PREFIX)/bin/archetypon"
	install -d "$(DESTDIR)$(PREFIX)/include"
	install -m 644 archetypon.h "$(DESTDIR)$(PREFIX)/include/archetypon.h"
	install -d "$(DESTDIR)$(PREFIX)/lib"
	install -m 644 $(LIBRARY) "$(DESTDIR)$(PREFIX)/lib/$(LIBRARY)"

test: archetypon build/api-test
	./build/api-test
	./tests/test.sh

clean:
	rm -rf build archetypon $(LIBRARY)
