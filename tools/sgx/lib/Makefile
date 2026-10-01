# Built for the device (armv7, musl): see ../README or
#   docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w cascadia-armdev make
CFLAGS ?= -O2 -Wall -D_FILE_OFFSET_BITS=64
sprites: sprites.c sgx2d.c sgx2d.h
	$(CC) $(CFLAGS) -o $@ sprites.c sgx2d.c -lm
clean:
	rm -f sprites
