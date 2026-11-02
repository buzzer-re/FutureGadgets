CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -pedantic
LDLIBS  += -lpthread -lm
PREFIX  ?= /usr/local

macdirstat: macdirstat.c
	$(CC) $(CFLAGS) -o $@ macdirstat.c $(LDFLAGS) $(LDLIBS)

install: macdirstat
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 macdirstat $(DESTDIR)$(PREFIX)/bin/macdirstat

clean:
	rm -f macdirstat

.PHONY: install clean
