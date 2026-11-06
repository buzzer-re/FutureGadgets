CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra
LDLIBS  += -lm -framework CoreFoundation -framework IOKit -framework Security -lIOReport
PREFIX  ?= $(HOME)/.local

mhtop: mhtop.c
	$(CC) $(CFLAGS) -o $@ mhtop.c $(LDFLAGS) $(LDLIBS)

install: mhtop
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 mhtop $(DESTDIR)$(PREFIX)/bin/mhtop

clean:
	rm -f mhtop

.PHONY: install clean
