CC      ?= cc
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra -Wno-unused-parameter
LDLIBS  := -lgmp -lmpfr -lz -lm
PREFIX  ?= /usr/local

OBJS = dictgen.o out.o calc.o

all: dictgen

dictgen: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c out.h calc.h
	$(CC) $(CFLAGS) -c $< -o $@

test: dictgen
	./tests/run.sh

install: dictgen
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 dictgen $(DESTDIR)$(PREFIX)/bin/dictgen

clean:
	rm -f $(OBJS) dictgen

.PHONY: all test install clean
