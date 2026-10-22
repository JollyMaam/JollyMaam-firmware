# Shared by every platform and product.

comma := ,
O := build
HOST_CC := gcc
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers
# Added to every board's -D flags, e.g. a test build:
# make gamesir-cyclone-2-pad EXTRA_DEFINES=-DUSB_SESSION_TEST_MS=12000
EXTRA_DEFINES :=
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

.PHONY: FORCE
