/* Minimal test harness: no external dependencies. */
#pragma once

#include <stdbool.h>
#include <stdio.h>

extern int test_failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            test_failures++;                                                           \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        long long _a = (long long)(a), _b = (long long)(b);                            \
        if (_a != _b) {                                                                \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,       \
                    __LINE__, #a, #b, _a, _b);                                         \
            test_failures++;                                                           \
        }                                                                              \
    } while (0)

typedef struct {
    const char *name;
    void (*fn)(void);
} test_case_t;

#define TEST(name) void name(void)
