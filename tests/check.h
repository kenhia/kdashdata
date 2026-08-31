/**
 * @file check.h
 * The whole test harness: a failure counter, a check macro, and a main()
 * epilogue. No framework, no dependency — these tests exist to be runnable by
 * `just check` on any host with a C compiler, and a harness that needed
 * installing would defeat that.
 */
#ifndef KDASH_TEST_CHECK_H
#define KDASH_TEST_CHECK_H

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define TEST_RESULT()                                                          \
    (failures ? (fprintf(stderr, "%d failure(s)\n", failures), 1)              \
              : (printf("ok\n"), 0))

#endif /* KDASH_TEST_CHECK_H */
