# Offline tests for the Swiss FTL core, against a raw dump of the NAND.
# Usage: make && ./nandtest <dump-dir> [--write | --rung3 | --rung4]
APPLE := ../../patches/files/drivers/mtd/nand/apple
CC ?= cc
CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -I$(APPLE) -I../nand-rung3 -I../nand-rung4 -std=gnu11
SRCS := nandtest.c $(APPLE)/sftl_map.c $(APPLE)/sftl_mount.c $(APPLE)/sftl_format.c $(APPLE)/sftl_write.c $(APPLE)/sftl_lwvm.c ../nand-rung3/rung3.c ../nand-rung4/rung4.c

nandtest: $(SRCS) $(wildcard $(APPLE)/*.h) ../nand-rung3/rung3.h ../nand-rung4/rung4.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f nandtest
