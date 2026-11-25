# rndis-probe: the extension's handshake, run from user space.
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ExorTek
#
# An ordinary macOS command-line tool. No entitlements, no signing beyond
# the ad-hoc one the linker applies, no SIP changes. It compiles the same
# rndis/ sources the extension does, so what it proves about them holds
# there too.

BUILD := build
PROBE := $(BUILD)/rndis-probe

ROOT   := ../..
CC     := $(shell xcrun --sdk macosx -f clang)
SDK    := $(shell xcrun --sdk macosx --show-sdk-path)
# Empty builds for this Mac only. A release passes "arm64 x86_64".
ARCHS  ?=
PREFIX ?= /usr/local

CFLAGS := -isysroot $(SDK) -mmacosx-version-min=13.0 -fobjc-arc \
          $(addprefix -arch ,$(ARCHS)) \
          -Wall -Wextra -Wno-unused-parameter -O2 -g \
          -I$(ROOT)/rndis -I../common -I../android-tether
LIBS   := -framework Foundation -framework IOKit -framework IOUSBHost

SRCS := main.m ../common/rndis_usb.m ../android-tether/netproto.c \
        $(ROOT)/rndis/rndis_control.c $(ROOT)/rndis/rndis_frame.c
HDRS := ../common/rndis_usb.h ../android-tether/netproto.h $(wildcard $(ROOT)/rndis/*.h)

.PHONY: all install uninstall clean
all: $(PROBE)

$(PROBE): $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) $(LIBS) -o $@

# Run as root (sudo make install): the result is then root's, which is what
# a program that will itself be run as root should be.
install: $(PROBE)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(PROBE) $(DESTDIR)$(PREFIX)/bin/rndis-probe

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/rndis-probe

clean:
	rm -rf $(BUILD)
