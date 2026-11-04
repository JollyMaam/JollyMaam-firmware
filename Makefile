# Each product's targets are in its make/<product>.mk.  Every board builds
# into $(O)/<target>/; host tools (built with HOST_CC) into $(O)/host/.
#   make            every product
#   make test       every product's unit tests
#   make flash      every device connected, through its vendor loader (see below)
#   make update     every device running our firmware, through our updater
#   make padtool-dist   padtool for Windows/Linux, with bleak, loaders and firmware
#   make dist       firmware, static desktop updater and SDK ZIPs
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
include make/8bitdo-ultimate2c-bluetooth.mk
include make/android.mk
include make/padtool.mk
include make/dist.mk

.PHONY: all test
all: gamesir-cyclone-2-all gamesir-nova-2-lite-all 8bitdo-ultimate2c-wireless-all
test: gamesir-cyclone-2-test gamesir-nova-2-lite-test 8bitdo-ultimate2c-wireless-test

# make package: what is built, as a firmware package in $(O)/firmware (jollymaam
# package); make flash / make update: that onto every device it fits, through
# the vendor loader / our updater (DEVICE=PORT: that one; WAIT=SECONDS for one
# to appear).  make BOARD... flash builds and flashes those boards only.
.PHONY: package flash update
FLASH_GOALS := $(filter-out package flash update,$(MAKECMDGOALS))
package: $(JOLLYMAAM_CLI) $(or $(FLASH_GOALS),all)
	$(Q)rm -rf $(O)/firmware
	$(Q)$(JOLLYMAAM_CLI) package $(O) $(O)/firmware
flash update: package
	$(Q)$(JOLLYMAAM_CLI) $(if $(filter flash,$@),oemflash,update) $(if $(DEVICE),--device $(DEVICE)) \
		$(if $(WAIT),--wait $(WAIT)) $(O)/firmware
