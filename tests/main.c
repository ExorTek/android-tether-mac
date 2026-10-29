/*
 * main.c -- run every test.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 */

#include <stdio.h>

#include "test_util.h"

int test_failures;
int test_checks;

int
main(void)
{
    printf("-- RNDIS data records --\n");
    run_frame_tests();
    printf("\n-- RNDIS control channel --\n");
    run_control_tests();

    printf("\n%d checks, %d failures\n", test_checks, test_failures);
    return test_failures == 0 ? 0 : 1;
}
