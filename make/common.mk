# Shared by every platform and product.

comma := ,
O := build
OF := $(abspath $O)
HOST_CC := gcc

HOST_CFLAGS :=
HOST_CFLAGS += -O2
HOST_CFLAGS += -g
HOST_CFLAGS += -std=c11
HOST_CFLAGS += -Wall
HOST_CFLAGS += -Wextra
HOST_CFLAGS += -Wno-unused-function
HOST_CFLAGS += -Wno-missing-field-initializers

# Added to every board's -D flags, e.g. a test build:
# make gamesir-cyclone-2-pad EXTRA_DEFINES=-DUSB_SESSION_TEST_MS=12000
EXTRA_DEFINES :=

# Our pads' tool device is the V-USB shared ID for mass storage, 16c0:27df,
# which requires its serial number to start with a domain or email address
# that we own, then a colon (usbdrv/USB-IDs-for-free.txt).  Placeholder
# until one is chosen.
TOOL_SERIAL_PREFIX := cyclone2-firmware

# Where the app's own headers are ("..." includes), for firmware and tests.
SRC_INCLUDES :=
SRC_INCLUDES += -iquote src/core
SRC_INCLUDES += -iquote src/profiles
SRC_INCLUDES += -iquote src/platform/jieli

# What changes the build: every object depends on these.
MAKE_FILES := Makefile $(wildcard make/*.mk)

BUILD_ID := $(shell git describe --always --dirty 2>/dev/null)
BUILD_REF := $(shell git symbolic-ref -q HEAD)
BUILD_METADATA := $(shell git rev-parse --git-path HEAD --git-path index $(if $(BUILD_REF),--git-path $(BUILD_REF)))

ifeq ($(V),1)
Q :=
else
Q := @
endif

# $(call board_defines,NAME): $(O)/NAME/defines holds the board's -D flags and
# is rewritten only when they change, so a test build rebuilds every object,
# and so does the next normal build.
define board_defines
$(O)/$(1)/defines: FORCE
	@mkdir -p $$(@D)
	@echo '$($(1)_DEFINES) $(EXTRA_DEFINES)' | cmp -s - $$@ 2>/dev/null || echo '$($(1)_DEFINES) $(EXTRA_DEFINES)' > $$@
endef

.PHONY: FORCE clean
clean:
	rm -rf $(O)
