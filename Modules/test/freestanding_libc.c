/*
 * freestanding_libc.c: the handful of libc functions real, non-throwaway CoreOS component
 * code (purr_relocate.c, purr_module.c, and more later) already calls by name (memcpy, from
 * <string.h>). A module builds -nostdlib -fno-builtin -- no libc linkage, and the compiler
 * won't even synthesize the call itself -- so anything a linked-in file calls explicitly needs
 * a real definition somewhere in the build. This is that somewhere: small, correct, freestanding
 * implementations, reusable by any future multi-file module (and eventually CoreOS itself,
 * PurrOS/components/coreos/SPEC.md section 4.1/8) that links in code written assuming libc.
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}
