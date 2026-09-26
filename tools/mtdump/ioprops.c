/* ioprops -- every property of the IOKit services of the given classes.
 *
 * Runs on the jailbroken iPad under iOS 8.4.1 (armv7), where there is no
 * ioreg.  Read only.  For the Wi-Fi bring-up: the HSIC bus interface, port and
 * chip services keep their state and I/O counters as properties.
 *
 *   ioprops CLASS [CLASS...]
 *   ioprops AppleBCMWLANBusInterfaceHSIC AppleUSBHSICPort
 */
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

typedef unsigned int io_object_t;
typedef int kern_return_t;
kern_return_t IOObjectRelease(io_object_t);
kern_return_t IORegistryEntryCreateCFProperties(io_object_t, CFMutableDictionaryRef *, CFAllocatorRef, unsigned int);
CFMutableDictionaryRef IOServiceMatching(const char *name);
kern_return_t IOServiceGetMatchingServices(unsigned int master, CFDictionaryRef matching, io_object_t *iter);
io_object_t IOIteratorNext(io_object_t iter);
kern_return_t IORegistryEntryGetPath(io_object_t, const char *plane, char *path);
kern_return_t IOObjectGetClass(io_object_t, char *name);

static void hexdump(const unsigned char *p, long n)
{
    long i, j;

    for(i = 0; i < n && i < 512; i += 16) {
        printf("      %04lx:", i);
        for(j = i; j < i + 16 && j < n; j++)
            printf(" %02x", p[j]);
        printf("\n");
    }
    if(n > 512)
        printf("      ... %ld bytes\n", n);
}

static void prop(const void *k, const void *v, void *arg)
{
    static char s[16384];
    char key[256];
    CFTypeID t = CFGetTypeID(v);

    (void)arg;
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

int main(int argc, char **argv)
{
    int i;

    if(argc < 2) {
        printf("usage: %s CLASS [CLASS...]\n", argv[0]);
        return 2;
    }
    for(i = 1; i < argc; i++) {
        io_object_t it = 0, e;
        int found = 0;

        if(IOServiceGetMatchingServices(0, IOServiceMatching(argv[i]), &it) || !it) {
            printf("== %s: no match\n", argv[i]);
            continue;
        }
        while((e = IOIteratorNext(it))) {
            CFMutableDictionaryRef props = NULL;
            char path[1024] = "", cls[128] = "";

            IORegistryEntryGetPath(e, "IOService", path);
            IOObjectGetClass(e, cls);
            printf("== %s  %s\n", cls, path);
            if(!IORegistryEntryCreateCFProperties(e, &props, kCFAllocatorDefault, 0) && props) {
                CFDictionaryApplyFunction(props, prop, NULL);
                CFRelease(props);
            }
            IOObjectRelease(e);
            found++;
        }
        IOObjectRelease(it);
        if(!found)
            printf("== %s: none\n", argv[i]);
    }
    return 0;
}
