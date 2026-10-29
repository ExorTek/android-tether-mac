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
# iig was given --framework-name, so the glue it generates includes the
# header as <AndroidTetherDriver/AndroidTetherDriver.h>. Generating into
# a directory of that name makes both that include and our own plain
# "AndroidTetherDriver.h" resolve, with no edits to generated code.
GENINC     := $(GEN)/AndroidTetherDriver
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

DEXT_DEPLOYMENT := 22.0

# Both architectures, so one download works on an Intel Mac and on Apple
# Silicon.
#
# On Apple Silicon the slice must be arm64e, not arm64: a plain arm64
# dext stages, activates, and then fails to launch, with a message that
# does not say why. Intel Macs take x86_64. The app is ordinary arm64 --
# arm64e is a dext requirement, not an application one.
#
# Which of these the SDK can still build for is asked of the SDK rather
# than assumed. Apple retires architectures as hardware ages, and a
# hard-coded x86_64 would turn into an opaque link failure the first
# time Xcode stops shipping it.
SDK_ARCHS := $(shell plutil -extract SupportedTargets.driverkit.Archs json -o - \
                 "$(DRIVERKIT_SDK)/SDKSettings.plist" 2>/dev/null | tr -d '[]" ' | tr ',' ' ')
DEXT_ARCHS := $(if $(strip $(SDK_ARCHS)),$(filter arm64e x86_64,$(SDK_ARCHS)),arm64e x86_64)
APP_ARCHS  := arm64 x86_64

ifeq ($(strip $(DEXT_ARCHS)),)
  $(error This DriverKit SDK reports no architecture we can build: $(SDK_ARCHS))
endif

DEXT_INCLUDES := \
	-isysroot $(DRIVERKIT_SDK) -I$(GEN) -I$(GENINC) -I. -I../rndis

IIG_CLANG_FLAGS := $(DEXT_INCLUDES) -x c++ -std=gnu++17 -D__IIG=1

# No -target here: it names exactly one architecture, so it is added
# per slice below.
DEXT_CXXFLAGS := \
	$(DEXT_INCLUDES) \
	-std=gnu++17 -fno-exceptions -fno-rtti -fno-builtin -fno-common \
	-Wall -Wextra -Wno-unused-parameter -O2 -g

DEXT_CFLAGS := \
	$(DEXT_INCLUDES) \
	-std=c11 -fno-builtin -fno-common -Wall -Wextra -O2 -g

# No -nostdlib, and no naming the DriverKit C runtime by hand. Clang's
# driverkit target knows which startup object and libraries a dext needs;
# -nostdlib only moves that job here, where the library's name has to be
# guessed and has in fact been spelled differently across SDKs.
# All three frameworks, not just DriverKit: USBDriverKit carries the
# IOUSBHostInterface metaclass and NetworkingDriverKit the
# IOUserNetworkEthernet vtable, both of which the generated glue
# references. Including a framework's headers does not link it.
DEXT_LDFLAGS := \
	-isysroot $(DRIVERKIT_SDK) \
	-framework DriverKit -framework USBDriverKit -framework NetworkingDriverKit

# The macOS driver understands several -arch flags in one invocation and
# produces a universal binary itself, so the app needs no lipo step.
APP_ARCHFLAGS := $(addprefix -arch ,$(APP_ARCHS))

APP_CFLAGS := \
	$(APP_ARCHFLAGS) -isysroot $(MACOS_SDK) -mmacosx-version-min=13.0 \
	-fobjc-arc -Wall -Wextra -Wno-unused-parameter -O2 -g
APP_LDFLAGS := \
	$(APP_ARCHFLAGS) -isysroot $(MACOS_SDK) -mmacosx-version-min=13.0 \
	-framework AppKit -framework Foundation -framework SystemExtensions

# ---- signing ------------------------------------------------------

SIGN_ID ?= -

# An ad-hoc build gets no entitlements on the app.
#
# The app's entitlements are restricted ones that only a provisioning
# profile can authorise. Signing an ad-hoc build with them does not
# merely fail to grant anything -- AMFI refuses to launch the binary at
# all, with "Launchd job spawn failed" and POSIX error 163, before any
# code runs. Leaving them off lets the unsigned build start and show
# its window, which is the only useful thing it could do anyway: the
# system extension still will not install.
#
# The extension keeps its entitlements either way. It is never launched
# directly, so they cost nothing there, and `codesign -d --entitlements`
# on it stays an honest statement of what the driver needs.
# ADHOC_ENTITLEMENTS=1 keeps them anyway. That is only useful on a
# machine with AMFI disabled, where entitlements are not checked and so
# an unauthorised one neither blocks the launch nor needs authorising.
# It is the development path for testing your own driver without an
# Apple Developer account; see README.md, including what it costs.
ifeq ($(SIGN_ID),-)
  ifeq ($(ADHOC_ENTITLEMENTS),1)
    APP_ENTITLEMENT_FLAGS := --entitlements $(BUILD)/Installer.entitlements
  else
    APP_ENTITLEMENT_FLAGS :=
  endif
else
  APP_ENTITLEMENT_FLAGS := --entitlements $(BUILD)/Installer.entitlements
endif

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

DEXT_OBJ_NAMES := \
	AndroidTetherDriver.o \
	AndroidTetherDriver_datapath.o \
	AndroidTetherDriver.iig.o \
	rndis_frame.o \
	rndis_control.o

# One slice per architecture, lipo'd together at the end.
DEXT_SLICES := $(foreach a,$(DEXT_ARCHS),$(BUILD)/slice-$(a))

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
#
# Only the header carries the recipe. A rule with two targets is two
# rules in GNU make, so writing both here would run iig once per target
# -- wasteful in series and a race under `make -j`, where two iig
# processes would write the same files at once.
$(GEN)/AndroidTetherDriver.iig.cpp: $(GENINC)/AndroidTetherDriver.h
	@:

$(GENINC)/AndroidTetherDriver.h: AndroidTetherDriver.iig
	@mkdir -p $(GENINC)
	$(IIG) --def AndroidTetherDriver.iig \
	       --header $(GENINC)/AndroidTetherDriver.h \
	       --impl $(GEN)/AndroidTetherDriver.iig.cpp \
	       --framework-name AndroidTetherDriver \
	       --deployment-target $(DEXT_DEPLOYMENT) \
	       -- $(IIG_CLANG_FLAGS)

# ---- the extension, once per architecture -------------------------
#
# iig output is architecture-independent and so is generated once; from
# there the same five objects are built for each slice into their own
# directory, so the two architectures cannot collide.
define dext_arch_rules
$(OBJ)/$(1)/%.o: %.cpp $(GENINC)/AndroidTetherDriver.h
	@mkdir -p $$(@D)
	$(CXX) -target $(1)-apple-driverkit$(DEXT_DEPLOYMENT) $(DEXT_CXXFLAGS) -c $$< -o $$@

$(OBJ)/$(1)/AndroidTetherDriver.iig.o: $(GEN)/AndroidTetherDriver.iig.cpp
	@mkdir -p $$(@D)
	$(CXX) -target $(1)-apple-driverkit$(DEXT_DEPLOYMENT) $(DEXT_CXXFLAGS) -c $$< -o $$@

$(OBJ)/$(1)/%.o: ../rndis/%.c
	@mkdir -p $$(@D)
	$(CXX) -x c -target $(1)-apple-driverkit$(DEXT_DEPLOYMENT) $(DEXT_CFLAGS) -c $$< -o $$@

$(BUILD)/slice-$(1): $$(addprefix $(OBJ)/$(1)/,$(DEXT_OBJ_NAMES))
	@mkdir -p $(BUILD)
	$(CXX) -target $(1)-apple-driverkit$(DEXT_DEPLOYMENT) $(DEXT_LDFLAGS) $$^ -o $$@
endef

$(foreach a,$(DEXT_ARCHS),$(eval $(call dext_arch_rules,$(a))))

$(BUILD)/AndroidTetherDriver.entitlements: AndroidTetherDriver.entitlements.in
	@mkdir -p $(BUILD)
	sed 's/__TEAM_ID__/$(TEAM_ID)/g' $< > $@

# A dext bundle is flat -- no Contents/, no MacOS/ -- and the executable
# has to be named after the bundle identifier, or OSSystemExtensionRequest
# will not find it.
dext: $(DEXT)
$(DEXT): $(DEXT_SLICES) Info.plist $(BUILD)/AndroidTetherDriver.entitlements
	@mkdir -p $(DEXT)
	@echo "  architectures: $(DEXT_ARCHS)"
	lipo -create $(DEXT_SLICES) -output $(DEXT)/$(DRIVER_ID)
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
	    $(APP_ENTITLEMENT_FLAGS) --options runtime $(APP)
ifeq ($(SIGN_ID),-)
ifeq ($(ADHOC_ENTITLEMENTS),1)
	@echo
	@echo "  Ad-hoc build WITH entitlements. This app will not launch unless"
	@echo "  AMFI is disabled on this machine. See README.md."
	@echo
else
	@echo
	@echo "  Ad-hoc build: the app is signed without its entitlements, so it"
	@echo "  will start and show its window. Installing the driver needs a"
	@echo "  provisioning profile, or a machine with AMFI disabled; see"
	@echo "  README.md."
	@echo
endif
endif

clean:
	rm -rf $(BUILD)
