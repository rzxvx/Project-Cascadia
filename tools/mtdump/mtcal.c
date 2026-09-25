/* mtcal -- this iPad's touch calibration, out of its own iOS, for Linux.
 *
 * Runs on the jailbroken iPad under iOS 8.4.1 (armv7) and writes the panel's
 * calibration, raw, to stdout: AppleMultitouchN1SPI's "Calibration Data",
 * which is what the driver downloads to the digitizer at 0x10009000 -- or,
 * without that service, the device tree's multi-touch-calibration, which
 * iBoot fills from syscfg MtCl at every boot.  On this iPad the two are the
 * same 1024 bytes.  Every panel has its own; ./cascadia mtcal runs this and
 * keeps the result as build/keep/lib/firmware/mtcal.bin.
 *
 * No SDK headers, on purpose: the few declarations it needs are below, so the
 * build image compiles and links it with clang and ld64.lld on any host
 * (tools/mtdump/build-mtcal.sh) and no Xcode is involved.
 */
typedef unsigned int io_object_t;
typedef int kern_return_t;
typedef const void *CFTypeRef;
typedef const void *CFStringRef;
typedef const void *CFDataRef;
typedef const void *CFAllocatorRef;
typedef void *CFMutableDictionaryRef;
typedef const void *CFDictionaryRef;
typedef unsigned long CFTypeID;
typedef long CFIndex;

#define kCFStringEncodingUTF8 0x08000100

extern const CFAllocatorRef kCFAllocatorDefault;
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, unsigned int);
CFTypeID CFGetTypeID(CFTypeRef);
CFTypeID CFDataGetTypeID(void);
CFIndex CFDataGetLength(CFDataRef);
const unsigned char *CFDataGetBytePtr(CFDataRef);
void CFRelease(CFTypeRef);

io_object_t IORegistryEntryFromPath(unsigned int master, const char *path);
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_object_t IOServiceGetMatchingService(unsigned int master, CFDictionaryRef matching);
CFTypeRef IORegistryEntryCreateCFProperty(io_object_t, CFStringRef key, CFAllocatorRef, unsigned int);
kern_return_t IOObjectRelease(io_object_t);

long write(int fd, const void *buf, unsigned long n);
unsigned long strlen(const char *);

static void say(const char *s)
{
    write(2, s, strlen(s));
}

/* Hex, by shifts: armv7 has no divide instruction, and a decimal loop would
 * pull in __divsi3 from libSystem for one number. */
static void say_hex(unsigned long n)
{
    char b[12], *p = b + sizeof(b);

    *--p = 0;
    do
        *--p = "0123456789abcdef"[n & 15];
    while((n >>= 4) && p > b + 2);
    *--p = 'x';
    *--p = '0';
    say(p);
}

/* The property as CFData, or 0.  The caller releases it. */
static CFDataRef get(io_object_t e, const char *key)
{
    CFStringRef k;
    CFTypeRef v;

    if(!e)
        return 0;
    k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    if(v && CFGetTypeID(v) != CFDataGetTypeID()) {
        CFRelease(v);
        v = 0;
    }
    return v;
}

int main(void)
{
    io_object_t svc, adt;
    CFDataRef d;
    const unsigned char *p;
    CFIndex n, i;
    long w;

    svc = IOServiceGetMatchingService(0, IOServiceMatching("AppleMultitouchN1SPI"));
    d = get(svc, "Calibration Data");
    if(d)
        say("mtcal: AppleMultitouchN1SPI \"Calibration Data\"");
    else {
        adt = IORegistryEntryFromPath(0, "IODeviceTree:/arm-io/spi1/multi-touch");
        d = get(adt, "multi-touch-calibration");
        if(d)
            say("mtcal: device tree multi-touch-calibration");
        if(adt)
            IOObjectRelease(adt);
    }
    if(svc)
        IOObjectRelease(svc);
    if(!d) {
        say("mtcal: no calibration here -- neither AppleMultitouchN1SPI nor arm-io/spi1/multi-touch has one\n");
        return 1;
    }

    p = CFDataGetBytePtr(d);
    n = CFDataGetLength(d);
    say(", ");
    say_hex(n);
    say(" bytes\n");
    for(i = 0; i < n; i += w) {
        w = write(1, p + i, n - i);
        if(w <= 0) {
            say("mtcal: write to stdout failed\n");
            return 1;
        }
    }
    CFRelease(d);
    return 0;
}
