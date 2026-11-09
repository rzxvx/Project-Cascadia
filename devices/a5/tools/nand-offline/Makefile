# Offline tests for the Swiss FTL core, against a raw dump of the NAND.
# Usage: make && ./nandtest <dump-dir> [--write]
APPLE := ../../patches/files/drivers/mtd/nand/apple
CC ?= cc
CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -I$(APPLE) -std=gnu11
SRCS := nandtest.c $(APPLE)/sftl_map.c $(APPLE)/sftl_mount.c $(APPLE)/sftl_format.c $(APPLE)/sftl_write.c $(APPLE)/sftl_lwvm.c

nandtest: $(SRCS) $(wildcard $(APPLE)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f nandtest
