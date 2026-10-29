# Each product's targets are in its make/<product>.mk.  Every board builds
# into $(O)/<target>/; host tools (built with HOST_CC) into $(O)/host/.
#   make            every product
#   make test       every product's unit tests
#   make padtool_dist   padtool for Windows/Linux, with bleak, loaders and firmware
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

.PHONY: all test padtool_dist
all: gamesir-cyclone-2-all gamesir-nova-2-lite-all 8bitdo-ultimate2c-wireless-all
test: gamesir-cyclone-2-test gamesir-nova-2-lite-test 8bitdo-ultimate2c-wireless-test

# padtool for Windows and Linux in $(O)/padtool-<build>.zip: padtool, bleak
# for 64-bit Windows and x86-64 Linux, Python 3.11-3.14 (fetched with uv), the RAM loaders and every board's
# firmware (tools/padtool_dist.py).
padtool_dist: $(CYCLONE2_PAD) $(CYCLONE2_DONGLE) $(NOVA2LITE_PAD) $(NOVA2LITE_DONGLE) $(U2CW_PAD)
	python3 tools/padtool_dist.py --build $(O) --version $(or $(BUILD_ID),dev) --out $(O)/padtool-$(or $(BUILD_ID),dev).zip
