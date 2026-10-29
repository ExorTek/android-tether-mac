# Top-level dispatcher.
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ExorTek
#
# `make test` runs anywhere clang does, including Linux CI, because the
# RNDIS protocol code has no platform in it. `make` additionally builds
# the driver, which needs macOS and the full Xcode.

.PHONY: all test asan driver clean

all: test driver

test:
	$(MAKE) -C tests

asan:
	$(MAKE) -C tests asan

driver:
	$(MAKE) -C driver

clean:
	$(MAKE) -C tests clean
	$(MAKE) -C driver clean
