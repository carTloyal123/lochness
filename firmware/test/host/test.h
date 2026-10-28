/* Minimal test harness: no external dependencies. */
#pragma once

#include <stdbool.h>

/* Record a check result; prints the failure. Use the macros below. */
void test_check(bool ok, const char *file, int line, const char *expr);
void test_check_eq(long long actual, long long expected, const char *file, int line,
                   const char *actual_expr, const char *expected_expr);
/* Number of failed checks so far. */
int test_failure_count(void);

#define CHECK(cond) test_check((cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(a, b) test_check_eq((long long)(a), (long long)(b), __FILE__, __LINE__, #a, #b)

typedef struct {
    const char *name;
    void (*fn)(void);
} test_case_t;

#define TEST(name) void name(void)
