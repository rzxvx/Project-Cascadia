# Built for the device (armv7, musl), in an Alpine arm/v7 container:
#   docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w cascadia-armdev make
# (cascadia-armdev: FROM alpine:3.24 on linux/arm/v7 + gcc musl-dev make)
CFLAGS ?= -O2 -Wall -D_FILE_OFFSET_BITS=64
all: sprites demo2 libsgxsdl.so
libsgxsdl.so: sgxsdl.c sgx2d.c sgx2d.h
	$(CC) $(CFLAGS) -shared -fPIC -o $@ sgxsdl.c sgx2d.c $$(sdl2-config --cflags --libs) -ldl -lm -pthread
%: %.c sgx2d.c sgx2d.h
	$(CC) $(CFLAGS) -o $@ $< sgx2d.c -lm
clean:
	rm -f sprites demo2 libsgxsdl.so
