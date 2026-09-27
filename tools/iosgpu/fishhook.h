// fishhook.h -- rebind symbols in Mach-O images at runtime.
// From facebook/fishhook (MIT).  Vendored for the iPad's armv7 iOS 8.4.1;
// used to intercept the IOKit calls iOS's GL driver makes to the kernel.
#ifndef FISHHOOK_H
#define FISHHOOK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rebinding {
    const char *name;
    void *replacement;
    void **replaced;
};

int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel);

#ifdef __cplusplus
}
#endif

#endif
