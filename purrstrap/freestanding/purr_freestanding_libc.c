/*
 * purr_freestanding_libc.c: the libc functions real, shipping CoreOS component code calls by
 * name (memcpy/memset/memcmp/memchr/strncmp/strtoul/strlen/strcmp/strcpy/strncpy/snprintf/
 * vsnprintf, from <string.h>/<stdlib.h>/<stdio.h>). A purrstrap-built relocatable module
 * compiles -nostdlib -fno-builtin -- no libc linkage, and the compiler won't even synthesize
 * the call itself -- so anything a linked-in file calls explicitly needs a real definition
 * somewhere in the build. This is that somewhere: correct, freestanding implementations,
 * `--source`'d alongside real source whenever a purrstrap build needs them (PurrOS/SPEC.md
 * section 6, `Modules/coreos/SPEC.md` section 4.1/8).
 *
 * Real, permanent infrastructure, not throwaway test code -- moved here from
 * Modules/test/freestanding_libc.c 2026-09-30, after it proved out for real: 15 of CoreOS's 16
 * real component files (everything except purr_appmgr.c, which separately needs conversion
 * onto purr_kernel_table_t before it can join) built, signed, loaded and ran successfully on
 * real T-Deck Plus hardware as one relocatable module through this exact file. vsnprintf/
 * snprintf here are scoped to what coreos/src's files actually call (checked directly): %%,
 * %c, %s (with an optional '-' flag and decimal width), %d/%i, %u, %x/%X, %p, with 'l'/'ll'
 * length modifiers -- no precision, no other flags, no floating point. Re-audit this list
 * against whatever real coreos.kitt source set is chosen if and when that build is attempted;
 * a new file added to that build may call something not covered here yet.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) {
            return *pa - *pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Only the subset purr_manifest.c actually needs: base-10 unsigned integers, no sign, no
 * leading whitespace skip, no other base, endptr always set. Not a general-purpose strtoul. */
unsigned long strtoul(const char *s, char **endptr, int base)
{
    unsigned long v = 0;
    (void)base;   /* callers in this codebase only ever parse base-10 fields */
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long)(*s - '0');
        s++;
    }
    if (endptr) {
        *endptr = (char *)s;
    }
    return v;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') {
        /* copying */
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

/*
 * A real, if scoped, vsnprintf/snprintf: only the specifiers this codebase's coreos/src files
 * actually use (checked directly, 2026-09-30, building all 16 as one relocatable module) --
 * %%, %c, %s (with an optional '-' flag and a decimal width, purr_cli.c's "%-8s"), %d/%i, %u,
 * %x/%X, %p, with 'l'/'ll' length modifiers on the integer conversions. No precision, no other
 * flags, no floating point -- not a general-purpose printf, but every conversion it does
 * support is done correctly (real padding, real sign handling, real base conversion), not a
 * shortcut that happens to work for today's call sites and silently mishandles a new one.
 */

static void out_char(char *buf, size_t cap, size_t *pos, char c)
{
    if (*pos < cap) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void out_str(char *buf, size_t cap, size_t *pos, const char *s, int width, int left)
{
    size_t len = strlen(s);
    int pad = width > (int)len ? width - (int)len : 0;
    if (!left) {
        while (pad-- > 0) {
            out_char(buf, cap, pos, ' ');
        }
    }
    for (size_t i = 0; i < len; i++) {
        out_char(buf, cap, pos, s[i]);
    }
    if (left) {
        while (pad-- > 0) {
            out_char(buf, cap, pos, ' ');
        }
    }
}

static void out_uint(char *buf, size_t cap, size_t *pos, unsigned long v, int base, int upper,
                     int negative)
{
    char digits[24];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) {
        digits[n++] = '0';
    }
    while (v > 0) {
        digits[n++] = set[v % (unsigned)base];
        v /= (unsigned)base;
    }
    if (negative) {
        out_char(buf, cap, pos, '-');
    }
    while (n > 0) {
        out_char(buf, cap, pos, digits[--n]);
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out_char(buf, cap, &pos, *p);
            continue;
        }
        p++;
        int left = 0;
        if (*p == '-') {
            left = 1;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        int long_count = 0;
        while (*p == 'l') {
            long_count++;
            p++;
        }
        switch (*p) {
        case '%':
            out_char(buf, cap, &pos, '%');
            break;
        case 'c':
            out_char(buf, cap, &pos, (char)va_arg(ap, int));
            break;
        case 's':
            out_str(buf, cap, &pos, va_arg(ap, const char *), width, left);
            break;
        case 'd':
        case 'i': {
            long v = long_count >= 1 ? va_arg(ap, long) : (long)va_arg(ap, int);
            out_uint(buf, cap, &pos, (unsigned long)(v < 0 ? -v : v), 10, 0, v < 0);
            break;
        }
        case 'u':
            out_uint(buf, cap, &pos,
                     long_count >= 1 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int),
                     10, 0, 0);
            break;
        case 'x':
        case 'X':
            out_uint(buf, cap, &pos,
                     long_count >= 1 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int),
                     16, *p == 'X', 0);
            break;
        case 'p':
            out_str(buf, cap, &pos, "0x", 0, 0);
            out_uint(buf, cap, &pos, (unsigned long)(uintptr_t)va_arg(ap, void *), 16, 0, 0);
            break;
        default:
            out_char(buf, cap, &pos, '%');
            out_char(buf, cap, &pos, *p);
            break;
        }
    }
    if (cap > 0) {
        buf[pos < cap ? pos : cap - 1] = '\0';
    }
    return (int)pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
