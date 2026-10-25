# Each product's targets are in its make/<product>.mk.  Every board builds
# into $(O)/<target>/; host tools (built with HOST_CC) into $(O)/host/.
#   make            every product
#   make test       every product's unit tests
#   make clean

.DEFAULT_GOAL := all
include make/common.mk
include make/host.mk
include make/btstack.mk
include make/jieli.mk
include make/telink.mk
include make/gamesir-cyclone-2.mk
include make/gamesir-nova-2-lite.mk
include make/8bitdo-ultimate2c-wireless.mk
include make/android.mk

.PHONY: all test
all: gamesir-cyclone-2-all gamesir-nova-2-lite-all 8bitdo-ultimate2c-wireless-all
test: gamesir-cyclone-2-test gamesir-nova-2-lite-test 8bitdo-ultimate2c-wireless-test
