/* mtdump -- what iOS knows about the P105 digitizer, for the Linux bring-up.
 *
 * Runs on the jailbroken iPad under iOS 8.4.1 (armv7).  Prints every property
 * of the live device-tree node arm-io/spi1/multi-touch -- as iBoot left it,
 * after its board-revision fixups and with syscfg's MtCl copied into
 * multi-touch-calibration -- and of the AppleMultitouchN1SPI service, whose
 * "Calibration Data" is what the driver downloads to 0x10009000.  The blobs
 * go to files as they are.
 *
 *   mtdump [outdir]        default outdir /tmp
 */
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

typedef unsigned int io_object_t;
typedef int kern_return_t;
kern_return_t IOObjectRelease(io_object_t);
io_object_t IORegistryEntryFromPath(unsigned int master, const char *path);
kern_return_t IORegistryEntryCreateCFProperties(io_object_t, CFMutableDictionaryRef *, CFAllocatorRef, unsigned int);
CFMutableDictionaryRef IOServiceMatching(const char *name);
kern_return_t IOServiceGetMatchingServices(unsigned int master, CFDictionaryRef matching, io_object_t *iter);
io_object_t IOIteratorNext(io_object_t iter);
kern_return_t IORegistryEntryGetPath(io_object_t, const char *plane, char *path);
kern_return_t IOObjectGetClass(io_object_t, char *name);
CFTypeRef IORegistryEntryCreateCFProperty(io_object_t, CFStringRef key, CFAllocatorRef, unsigned int);

static const char *outdir = "/tmp";

static void save(const char *what, const char *key, CFDataRef d)
{
    char path[512], *p;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s-%s.bin", outdir, what, key);
    for(p = path + strlen(outdir) + 1; *p; p++)
        if(*p == ' ' || *p == '/' || *p == ',')
            *p = '_';
    f = fopen(path, "wb");
    if(!f) {
        printf("      (could not write %s)\n", path);
        return;
    }
    fwrite(CFDataGetBytePtr(d), 1, CFDataGetLength(d), f);
    fclose(f);
    printf("      -> %s\n", path);
}

static void hexdump(const unsigned char *p, long n)
{
    long i, j, last_nz = -1;

    for(i = 0; i < n; i++)
        if(p[i])
            last_nz = i;
    for(i = 0; i < n; i += 16) {
        if(i > last_nz + 16 && i + 16 < n) {
            printf("      %04lx: ... zeros to the end (%ld bytes)\n", i, n);
            return;
        }
        printf("      %04lx:", i);
        for(j = i; j < i + 16 && j < n; j++)
            printf(" %02x", p[j]);
        printf("\n");
    }
}

struct ctx { const char *what; };

static void prop(const void *k, const void *v, void *arg)
{
    struct ctx *c = arg;
    char key[256], s[1024];
    CFTypeID t = CFGetTypeID(v);

    if(!CFStringGetCString(k, key, sizeof(key), kCFStringEncodingUTF8))
        strcpy(key, "?");
    if(t == CFDataGetTypeID()) {
        const unsigned char *p = CFDataGetBytePtr(v);
        long n = CFDataGetLength(v), i, printable = n > 0;
        for(i = 0; i < n; i++)
            if(!(p[i] >= 0x20 && p[i] < 0x7f) && !(p[i] == 0 && i == n - 1))
                printable = 0;
        if(printable)
            printf("  %-32s \"%.*s\"\n", key, (int)(p[n - 1] ? n : n - 1), p);
        else {
            printf("  %-32s data, %ld bytes\n", key, n);
            hexdump(p, n);
            if(n >= 64 || strstr(key, "alibration"))
                save(c->what, key, v);
        }
    } else if(t == CFStringGetTypeID()) {
        CFStringGetCString(v, s, sizeof(s), kCFStringEncodingUTF8);
        printf("  %-32s \"%s\"\n", key, s);
    } else if(t == CFNumberGetTypeID()) {
        long long x = 0;
        CFNumberGetValue(v, kCFNumberSInt64Type, &x);
        printf("  %-32s %lld (0x%llx)\n", key, x, x);
    } else if(t == CFBooleanGetTypeID()) {
        printf("  %-32s %s\n", key, CFBooleanGetValue(v) ? "true" : "false");
    } else {
        CFStringRef d = CFCopyDescription(v);
        if(d && CFStringGetCString(d, s, sizeof(s), kCFStringEncodingUTF8))
            printf("  %-32s %s\n", key, s);
        else
            printf("  %-32s (type %lu)\n", key, (unsigned long)t);
        if(d)
            CFRelease(d);
    }
}

static void dump(const char *what, io_object_t e)
{
    CFMutableDictionaryRef props = NULL;
    struct ctx c = { what };
    char path[1024] = "";

    IORegistryEntryGetPath(e, "IOService", path);
    if(!path[0])
        IORegistryEntryGetPath(e, "IODeviceTree", path);
    printf("== %s  %s\n", what, path);
    if(IORegistryEntryCreateCFProperties(e, &props, kCFAllocatorDefault, 0) || !props) {
        printf("  (no properties)\n");
        return;
    }
    CFDictionaryApplyFunction(props, prop, &c);
    CFRelease(props);
}

int main(int argc, char **argv)
{
    static const char *nodes[] = {
        "IODeviceTree:/arm-io/spi1/multi-touch",
        "IODeviceTree:/arm-io/spi1",
        "IODeviceTree:/arm-io/pwm",
        "IODeviceTree:/arm-io/pwm/grape-clk",
    };
    static const char *classes[] = {
        "AppleMultitouchN1SPI", "AppleMultitouchZ2SPI", "AppleMultitouchSPI",
    };
    unsigned i;
    io_object_t e, it;
    int found = 0;

    if(argc > 1)
        outdir = argv[1];
    setvbuf(stdout, NULL, _IOLBF, 0);

    for(i = 0; i < sizeof(nodes) / sizeof(*nodes); i++) {
        e = IORegistryEntryFromPath(0, nodes[i]);
        if(!e) {
            printf("== %s: not found\n", nodes[i]);
            continue;
        }
        dump(i == 0 ? "adt-multi-touch" : nodes[i] + 13, e);
        IOObjectRelease(e);
    }

    for(i = 0; i < sizeof(classes) / sizeof(*classes) && !found; i++) {
        if(IOServiceGetMatchingServices(0, IOServiceMatching(classes[i]), &it))
            continue;
        while((e = IOIteratorNext(it))) {
            dump(classes[i], e);
            IOObjectRelease(e);
            found = 1;
        }
        IOObjectRelease(it);
    }
    if(!found)
        printf("== no AppleMultitouch*SPI service found\n");

    /* The PMU, live: AppleD1946PMUPowerSource computes these on request
     * (getProperty override, 0x80c917c0) -- "AppleRegisterDump" is PMU
     * registers 0x00..0x7f read there and then (_readRegs(0, 0x80)), so it
     * is not in the property table and has to be asked for by name.  Which
     * registered class carries it is looked for, not assumed. */
    {
        static const char *keys[] = { "AppleRegisterDump", "AppleRawBatteryVoltage", "AppleVoltageDictionary" };
        static const char *pmucls[] = { "AppleD1946PMUPowerSource", "IOPMPowerSource", "AppleD1946PMU" };
        unsigned k, c2, n;
        char cls[128];
        for(c2 = 0; c2 < sizeof(pmucls) / sizeof(*pmucls); c2++) {
            if(IOServiceGetMatchingServices(0, IOServiceMatching(pmucls[c2]), &it)) {
                printf("== %s: matching failed\n", pmucls[c2]);
                continue;
            }
            n = 0;
            while((e = IOIteratorNext(it))) {
                struct ctx c = { "pmu" };
                cls[0] = 0;
                IOObjectGetClass(e, cls);
                printf("== %s match: class %s\n", pmucls[c2], cls);
                n++;
                for(k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
                    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, keys[k], kCFStringEncodingUTF8);
                    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
                    if(v) {
                        prop(key, v, &c);
                        CFRelease(v);
                    } else
                        printf("  %-32s (none)\n", keys[k]);
                    CFRelease(key);
                }
                IOObjectRelease(e);
            }
            IOObjectRelease(it);
            if(!n)
                printf("== %s: no service\n", pmucls[c2]);
        }
    }
    return 0;
}
