# android-tether: internet from the phone, from user space.
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ExorTek
#
# An ordinary macOS command-line tool, run with sudo. No entitlements and
# no SIP changes: root is enough for a utun interface and routes, and
# nothing needs root to open the phone's RNDIS interface.

BUILD  := build
TETHER := $(BUILD)/android-tether

ROOT   := ../..
CC     := $(shell xcrun --sdk macosx -f clang)
SDK    := $(shell xcrun --sdk macosx --show-sdk-path)
# Empty builds for this Mac only. A release passes "arm64 x86_64".
ARCHS  ?=
PREFIX ?= /usr/local

CFLAGS := -isysroot $(SDK) -mmacosx-version-min=13.0 -fobjc-arc \
          $(addprefix -arch ,$(ARCHS)) \
          -Wall -Wextra -Wno-unused-parameter -O2 -g \
          -I$(ROOT)/rndis -I../common -I.
LIBS   := -framework Foundation -framework IOKit -framework IOUSBHost \
          -framework SystemConfiguration

SRCS := main.m netconfig.m netproto.c utun.c ../common/rndis_usb.m \
        $(ROOT)/rndis/rndis_control.c $(ROOT)/rndis/rndis_frame.c
HDRS := netconfig.h netproto.h utun.h ../common/rndis_usb.h $(wildcard $(ROOT)/rndis/*.h)

.PHONY: all install uninstall clean
all: $(TETHER)

$(TETHER): $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) $(LIBS) -o $@

# Run as root (sudo make install): the result is then root's, which is what
# a program that will itself be run as root should be.
install: $(TETHER)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TETHER) $(DESTDIR)$(PREFIX)/bin/android-tether

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/android-tether

clean:
	rm -rf $(BUILD)
