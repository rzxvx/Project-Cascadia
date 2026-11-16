# Offline tests for the Swiss FTL core, against a raw dump of the NAND.
# Usage: make && ./nandtest <dump-dir> [--write | --rung3 | --rung4 | --rung5 | --rung6 | --rung7 | --lwvm FROM TO | --recover]
APPLE := ../../patches/files/drivers/mtd/nand/apple
CC ?= cc
CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -I$(APPLE) -I../nand-rung3 -I../nand-rung4 -I../nand-rung5 -I../nand-rung6 -I../nand-rung7 -I../nand-volume -I../nand-recover -std=gnu11
SRCS := nandtest.c $(APPLE)/sftl_map.c $(APPLE)/sftl_mount.c $(APPLE)/sftl_format.c $(APPLE)/sftl_write.c $(APPLE)/sftl_lwvm.c ../nand-rung3/rung3.c ../nand-rung4/rung4.c ../nand-rung5/rung5.c ../nand-rung6/rung6.c ../nand-rung7/rung7.c ../nand-volume/lwvmw.c ../nand-recover/recover.c

nandtest: $(SRCS) $(wildcard $(APPLE)/*.h) ../nand-rung3/rung3.h ../nand-rung4/rung4.h ../nand-rung5/rung5.h ../nand-rung6/rung6.h ../nand-rung7/rung7.h ../nand-volume/lwvmw.h ../nand-recover/recover.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f nandtest
