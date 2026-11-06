BUILD_ID := $(shell git describe --always --dirty 2>/dev/null || echo "unknown")

O := build
OF := $(abspath $O)

HOST_CC ?= $(if $(filter default undefined,$(origin CC)),gcc,$(CC))
HOST_AR ?= $(AR)
HOST_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers

EXTRA_DEFINES :=
SRC_INCLUDES := -iquote src/core -iquote src/profiles -iquote src/platform/jieli

include src/make/lib.mk
include src/make/android.mk
include src/make/btstack.mk
include src/make/dist.mk
include src/make/host-tests.mk
include src/make/jieli.mk
include src/make/padtool.mk
include src/make/sources.mk
include src/make/telink-lib.mk
include src/make/telink.mk
include src/make/yc3121-lib.mk

RULES += src/core
RULES += src/lib
RULES += tools
RULES += tools/jollymaam
RULES += src/boards/gamesir-cyclone-2
RULES += src/boards/gamesir-nova-2-lite
RULES += src/boards/8bitdo-ultimate2c-wireless
RULES += src/boards/8bitdo-ultimate2c-bluetooth

$(eval $(call generate-rules,$(RULES)))
