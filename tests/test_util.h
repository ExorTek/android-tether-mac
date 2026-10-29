/*
 * test_util.h -- the whole test harness.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * Deliberately tiny and dependency-free: these tests have to build with
 * nothing but a C compiler, on any machine, so that the one testable part
 * of a driver nobody can run stays testable.
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond, ...) do {                                           \
        test_checks++;                                                  \
        if (!(cond)) {                                                  \
            test_failures++;                                            \
            printf("  FAIL %s:%d: ", __func__, __LINE__);               \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

#define RUN(fn, name) do {                                              \
        const int before = test_failures;                               \
        fn();                                                           \
        printf("%s %s\n", test_failures == before ? "ok  " : "FAIL", name); \
    } while (0)

void run_frame_tests(void);
void run_control_tests(void);

#endif /* TEST_UTIL_H */
