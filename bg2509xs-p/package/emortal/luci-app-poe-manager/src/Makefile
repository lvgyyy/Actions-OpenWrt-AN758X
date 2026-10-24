# SPDX-License-Identifier: GPL-2.0-only

CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Werror
LDLIBS += -luci
OBJECTS := main.o poe.o rtl8238b.o log.o log_sink.o

.PHONY: compile install clean
compile: poe-manager

poe-manager: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJECTS): poe.h log.h log_sink.h

rtl8238b.o: rtl8238b_fw.h

install: compile
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 poe-manager $(DESTDIR)/usr/sbin/poe-manager

clean:
	rm -f poe-manager $(OBJECTS)
