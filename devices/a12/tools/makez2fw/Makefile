# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
MXML_CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += -Imxml-3.1
LDLIBS += -pthread

MXML_LIB = mxml-3.1/libmxml.a
OBJS = makez2fw.o qdict.o eplist.o syscfg.o mtfw.o

.PHONY: all clean

all: makez2fw

makez2fw: $(OBJS) $(MXML_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(MXML_LIB) $(LDLIBS)

$(MXML_LIB):
	$(MAKE) -C mxml-3.1 CC="$(CC)" AR="$(AR)" CFLAGS="$(MXML_CFLAGS)"

clean:
	rm -f makez2fw $(OBJS)
	$(MAKE) -C mxml-3.1 clean
