# Shared by every platform and product.

comma := ,
O := build
OF := $(abspath $O)
HOST_CC ?= $(if $(filter default undefined,$(origin CC)),gcc,$(CC))
HOST_AR ?= $(AR)

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

# Where the app's own headers are ("..." includes), for firmware and tests.
SRC_INCLUDES :=
SRC_INCLUDES += -iquote src/core
SRC_INCLUDES += -iquote src/profiles
SRC_INCLUDES += -iquote src/platform/jieli

# The build's name in the firmware (src/core/rules.mk's build_id.h).
BUILD_ID := $(shell git describe --always --dirty 2>/dev/null)

ifeq ($(V),1)
Q :=
else
Q := @
endif

.PHONY: FORCE clean
clean:
	rm -rf $(O)
