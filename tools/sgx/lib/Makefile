# Built for the device (armv7, musl).  ./cascadia gpu cross-builds the three
# that ship (cross.sh, no ARM emulation needed).  For the diagnostics too, in
# an Alpine arm/v7 container, on a host that can run one:
#   docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w cascadia-armdev make
# (cascadia-armdev: Dockerfile here; FROM alpine:3.24 + gcc musl-dev make ...)
CFLAGS ?= -O2 -Wall -D_FILE_OFFSET_BITS=64
all: sprites demo2 libsgxsdl.so
libsgxsdl.so: sgxsdl.c sgx2d.c sgx2d.h
	$(CC) $(CFLAGS) -shared -fPIC -o $@ sgxsdl.c sgx2d.c $$(sdl2-config --cflags --libs) -ldl -lm -pthread
%: %.c sgx2d.c sgx2d.h
	$(CC) $(CFLAGS) -o $@ $< sgx2d.c -lm
clean:
	rm -f sprites demo2 libsgxsdl.so
