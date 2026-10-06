CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c99 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DESTDIR ?=
INSTALL ?= install

.PHONY: all clean test install uninstall test-install

all: plycount

plycount: plycount.c
	$(CC) $(CFLAGS) $(WARNINGS) -D_POSIX_C_SOURCE=200809L -o $@ $<

test: plycount
	CC='$(CC)' sh tests/test_cli.sh

install: plycount
	$(INSTALL) -d -m 755 '$(DESTDIR)$(BINDIR)' '$(DESTDIR)$(MANDIR)/man1'
	$(INSTALL) -m 755 plycount '$(DESTDIR)$(BINDIR)/plycount'
	$(INSTALL) -m 644 doc/plycount.1 '$(DESTDIR)$(MANDIR)/man1/plycount.1'

uninstall:
	rm -f '$(DESTDIR)$(BINDIR)/plycount' '$(DESTDIR)$(MANDIR)/man1/plycount.1'

test-install: plycount
	MAKE='$(MAKE)' sh tests/test_install.sh

clean:
	rm -f plycount
