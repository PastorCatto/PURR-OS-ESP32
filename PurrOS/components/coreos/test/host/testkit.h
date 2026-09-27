/* testkit.h - a tiny assertion kit for the host tests. No dependencies. */
#ifndef TESTKIT_H
#define TESTKIT_H

#include <stdio.h>

static int tk_total, tk_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        tk_total++;                                                          \
        if (!(cond)) {                                                       \
            tk_failed++;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long tk_a = (long long)(a), tk_b = (long long)(b);              \
        tk_total++;                                                          \
        if (tk_a != tk_b) {                                                  \
            tk_failed++;                                                     \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,      \
                   __LINE__, #a, #b, tk_a, tk_b);                            \
        }                                                                    \
    } while (0)

#define TK_DONE(name)                                                        \
    do {                                                                     \
        printf("%s: %d checks, %d failed\n", name, tk_total, tk_failed);     \
        return tk_failed ? 1 : 0;                                            \
    } while (0)

#endif /* TESTKIT_H */
