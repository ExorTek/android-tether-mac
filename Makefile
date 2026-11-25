# Top-level dispatcher.
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ExorTek
#
# `make test` runs anywhere clang does, including Linux CI, because the
# RNDIS protocol code has no platform in it. `make` additionally builds
# the driver, which needs macOS and the full Xcode.

.PHONY: all test asan driver probe tether install uninstall clean

all: test driver

# The extension's handshake run from user space against a real phone.
# macOS only, but needs neither entitlements nor SIP changes.
probe:
	$(MAKE) -C tools/rndis-probe

# Internet from the phone today, from user space: run the result with sudo.
tether:
	$(MAKE) -C tools/android-tether

# Both tools into $(PREFIX)/bin, /usr/local by default: sudo make install.
install:
	$(MAKE) -C tools/android-tether install
	$(MAKE) -C tools/rndis-probe install

uninstall:
	$(MAKE) -C tools/android-tether uninstall
	$(MAKE) -C tools/rndis-probe uninstall

test:
	$(MAKE) -C tests

asan:
	$(MAKE) -C tests asan

driver:
	$(MAKE) -C driver

clean:
	$(MAKE) -C tests clean
	$(MAKE) -C driver clean
	$(MAKE) -C tools/rndis-probe clean
	$(MAKE) -C tools/android-tether clean
