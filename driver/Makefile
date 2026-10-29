# Builds the system extension and the app that carries it, without an
# Xcode project.
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ExorTek
#
# There is no .xcodeproj on purpose: the whole build is nine commands, and
# a project file hides which of them matters. What it does need is the
# full Xcode, not the Command Line Tools -- the DriverKit SDK and `iig`
# ship only with Xcode.
#
# Usage:
#   make                 build both bundles, ad-hoc signed
#   make SIGN_ID="..."   sign with a Developer ID Application identity
#   make clean
#
# The Team ID is never committed. It is read from a provisioning profile
# in this directory if there is one, and otherwise left as a placeholder,
# which is fine for an ad-hoc build that is not going to load anyway.

DRIVER_ID  := com.exortek.androidtether.driver
APP_ID     := com.exortek.androidtether
APP_NAME   := AndroidTether

BUILD      := build
OBJ        := $(BUILD)/obj
GEN        := $(BUILD)/gen
DEXT       := $(BUILD)/$(DRIVER_ID).dext
APP        := $(BUILD)/$(APP_NAME).app

# ---- toolchain ----------------------------------------------------

# Fail with something a person can act on rather than a missing-file
# error forty lines down.
ifeq ($(shell command -v xcodebuild >/dev/null 2>&1 && echo yes),)
  $(error xcodebuild not found. This build needs the full Xcode, not just \
the Command Line Tools. Install Xcode, then run: \
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer)
endif

DRIVERKIT_SDK := $(shell xcrun --sdk driverkit --show-sdk-path 2>/dev/null)
ifeq ($(DRIVERKIT_SDK),)
  $(error No DriverKit SDK. `xcrun --sdk driverkit --show-sdk-path` found \
nothing, which usually means xcode-select still points at the Command \
Line Tools. Run: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer)
endif

MACOS_SDK := $(shell xcrun --sdk macosx --show-sdk-path)
IIG       := $(shell xcrun --sdk driverkit -f iig)
CXX       := $(shell xcrun --sdk driverkit -f clang++)
CC        := $(shell xcrun --sdk macosx -f clang)
CODESIGN  := $(shell xcrun -f codesign)

# arm64e is not optional. A dext built for plain arm64 stages, activates,
# and then fails to launch -- with a message that does not say why.
DEXT_DEPLOYMENT := 22.0
DEXT_TARGET     := arm64e-apple-driverkit$(DEXT_DEPLOYMENT)

DEXT_INCLUDES := \
	-isysroot $(DRIVERKIT_SDK) -I$(GEN) -I. -I../rndis

IIG_CLANG_FLAGS := $(DEXT_INCLUDES) -x c++ -std=gnu++17 -D__IIG=1

DEXT_CXXFLAGS := \
	-target $(DEXT_TARGET) $(DEXT_INCLUDES) \
	-std=gnu++17 -fno-exceptions -fno-rtti -fno-builtin -fno-common \
	-Wall -Wextra -Wno-unused-parameter -O2 -g

DEXT_CFLAGS := \
	-target $(DEXT_TARGET) $(DEXT_INCLUDES) \
	-std=c11 -fno-builtin -fno-common -Wall -Wextra -O2 -g

# The DriverKit C runtime has been named both ways across SDKs; pick
# whichever this one actually ships rather than hard-coding a guess that
# breaks on the next Xcode.
DEXT_CRT := $(if $(wildcard $(DRIVERKIT_SDK)/System/DriverKit/usr/lib/libCRTEndClient_driverkit.a),\
                 -lCRTEndClient_driverkit,-lCRTEndClient)

DEXT_LDFLAGS := \
	-target $(DEXT_TARGET) -isysroot $(DRIVERKIT_SDK) \
	-framework DriverKit -nostdlib $(DEXT_CRT)

APP_CFLAGS := \
	-isysroot $(MACOS_SDK) -mmacosx-version-min=13.0 \
	-fobjc-arc -Wall -Wextra -Wno-unused-parameter -O2 -g
APP_LDFLAGS := \
	-isysroot $(MACOS_SDK) -mmacosx-version-min=13.0 \
	-framework AppKit -framework Foundation -framework SystemExtensions

# ---- signing ------------------------------------------------------

SIGN_ID ?= -

# A .mobileprovision dropped next to this Makefile carries the Team ID;
# pull it out rather than asking anyone to edit a file they would then
# have to remember not to commit.
PROFILE := $(wildcard *.mobileprovision)
ifneq ($(PROFILE),)
  TEAM_ID := $(shell security cms -D -i $(firstword $(PROFILE)) 2>/dev/null | \
      plutil -extract TeamIdentifier.0 raw - 2>/dev/null)
endif
TEAM_ID ?= 0000000000

# ---- sources ------------------------------------------------------

DEXT_CXX_SRCS := AndroidTetherDriver.cpp AndroidTetherDriver_datapath.cpp
DEXT_C_SRCS   := ../rndis/rndis_frame.c ../rndis/rndis_control.c

DEXT_OBJS := \
	$(OBJ)/AndroidTetherDriver.o \
	$(OBJ)/AndroidTetherDriver_datapath.o \
	$(OBJ)/AndroidTetherDriver.iig.o \
	$(OBJ)/rndis_frame.o \
	$(OBJ)/rndis_control.o

.PHONY: all dext app clean
all: app

# ---- the generated interface --------------------------------------

# iig turns the .iig into a header the driver includes and a .cpp holding
# the dispatch glue. Both are generated; neither is committed.
#
# Two things about the flags after the `--`, both learned the hard way:
#
#  * -D__IIG=1 is required. The DriverKit headers gate their annotation
#    macros -- KERNEL, NATIVE, EXTENDS, LOCALONLY -- on it. Without it
#    those expand to nothing recognisable and iig fails *inside Apple's
#    own OSObject.iig* with "variable has incomplete type 'class
#    KERNEL'", which reads like a broken SDK and is not.
#
#  * -target must NOT be passed. iig parses it itself and rejects it
#    with "-target is only permitted with an unknown arch". The platform
#    comes from --deployment-target instead. The compiler proper does
#    get -target; only iig does not.
$(GEN)/AndroidTetherDriver.h $(GEN)/AndroidTetherDriver.iig.cpp: AndroidTetherDriver.iig
	@mkdir -p $(GEN)
	$(IIG) --def AndroidTetherDriver.iig \
	       --header $(GEN)/AndroidTetherDriver.h \
	       --impl $(GEN)/AndroidTetherDriver.iig.cpp \
	       --framework-name AndroidTetherDriver \
	       --deployment-target $(DEXT_DEPLOYMENT) \
	       -- $(IIG_CLANG_FLAGS)

# ---- the extension ------------------------------------------------

$(OBJ)/%.o: %.cpp $(GEN)/AndroidTetherDriver.h
	@mkdir -p $(OBJ)
	$(CXX) $(DEXT_CXXFLAGS) -c $< -o $@

$(OBJ)/AndroidTetherDriver.iig.o: $(GEN)/AndroidTetherDriver.iig.cpp
	@mkdir -p $(OBJ)
	$(CXX) $(DEXT_CXXFLAGS) -c $< -o $@

$(OBJ)/%.o: ../rndis/%.c
	@mkdir -p $(OBJ)
	$(CXX) -x c $(DEXT_CFLAGS) -c $< -o $@

$(BUILD)/AndroidTetherDriver.entitlements: AndroidTetherDriver.entitlements.in
	@mkdir -p $(BUILD)
	sed 's/__TEAM_ID__/$(TEAM_ID)/g' $< > $@

# A dext bundle is flat -- no Contents/, no MacOS/ -- and the executable
# has to be named after the bundle identifier, or OSSystemExtensionRequest
# will not find it.
dext: $(DEXT)
$(DEXT): $(DEXT_OBJS) Info.plist $(BUILD)/AndroidTetherDriver.entitlements
	@mkdir -p $(DEXT)
	$(CXX) $(DEXT_LDFLAGS) $(DEXT_OBJS) -o $(DEXT)/$(DRIVER_ID)
	cp Info.plist $(DEXT)/Info.plist
	$(CODESIGN) --force --sign "$(SIGN_ID)" --timestamp=none \
	    --entitlements $(BUILD)/AndroidTetherDriver.entitlements \
	    --options runtime $(DEXT)

# ---- the app ------------------------------------------------------

$(OBJ)/main.o: ../app/main.m
	@mkdir -p $(OBJ)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/Installer.entitlements: ../app/Installer.entitlements.in
	@mkdir -p $(BUILD)
	sed 's/__TEAM_ID__/$(TEAM_ID)/g' $< > $@

app: $(APP)
$(APP): $(OBJ)/main.o ../app/Info.plist $(BUILD)/Installer.entitlements $(DEXT)
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Library/SystemExtensions
	$(CC) $(APP_LDFLAGS) $(OBJ)/main.o -o $(APP)/Contents/MacOS/$(APP_NAME)
	cp ../app/Info.plist $(APP)/Contents/Info.plist
	rm -rf $(APP)/Contents/Library/SystemExtensions/$(DRIVER_ID).dext
	cp -R $(DEXT) $(APP)/Contents/Library/SystemExtensions/
	$(CODESIGN) --force --sign "$(SIGN_ID)" --timestamp=none \
	    --entitlements $(BUILD)/Installer.entitlements \
	    --options runtime $(APP)

clean:
	rm -rf $(BUILD)
