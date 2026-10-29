# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ExorTek
#
# No Xcode, no SDK, no hardware: these build and run anywhere a C compiler
# does, which is the point of keeping rndis/ free of platform headers.
#
#   make -C tests          build and run
#   make -C tests asan     the same, under the address and UB sanitizers

CC      ?= cc
RNDIS   := ../rndis
CFLAGS  += -std=c11 -Wall -Wextra -Werror -I$(RNDIS) -I.
BUILD   := build

SRCS    := main.c test_frame.c test_control.c \
           $(RNDIS)/rndis_frame.c $(RNDIS)/rndis_control.c
HDRS    := test_util.h $(RNDIS)/rndis.h $(RNDIS)/rndis_frame.h \
           $(RNDIS)/rndis_control.h

.PHONY: all run asan clean

all: run

$(BUILD)/tests: $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: $(BUILD)/tests
	./$(BUILD)/tests

# Worth running whenever the parsing changes: it reads lengths chosen by the
# device, so a bounds mistake should fail loudly here rather than quietly on
# someone's Mac.
asan: $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/tests_asan $(SRCS)
	./$(BUILD)/tests_asan

clean:
	rm -rf $(BUILD)
