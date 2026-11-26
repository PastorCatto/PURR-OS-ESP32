/* the few libc functions the link code uses: a module has none (Modules/SPEC.md), so they are here, plain loops the compiler must not turn back into calls */
#include <stddef.h>
#include <stdint.h>

__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; for (size_t i = 0; i < n; i++) a[i] = b[i]; return d; }
__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a < b) { for (size_t i = 0; i < n; i++) a[i] = b[i]; } else { for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1]; }
    return d;
}
__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memset(void *d, int c, size_t n) { uint8_t *a = d; for (size_t i = 0; i < n; i++) a[i] = (uint8_t)c; return d; }
__attribute__((optimize("no-tree-loop-distribute-patterns"))) int memcmp(const void *x, const void *y, size_t n) { const uint8_t *a = x, *b = y; for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; } return 0; }
__attribute__((optimize("no-tree-loop-distribute-patterns"))) size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strncmp(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (uint8_t)a[i] < (uint8_t)b[i] ? -1 : 1; if (!a[i]) return 0; } return 0; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
