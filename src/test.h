#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>


static int test_failures;

#define CHECK(c) do { \
    if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); test_failures++; } \
} while (0)

#define TEST_DONE(name) \
    (test_failures ? 1 : (printf("%s: ok\n", name), 0))

#endif
