# Built with make/lib.mk: each directory in RULES has a rules.mk with its
# targets; everything builds into $(O) (OBJDIR), a board into $(O)/<board>/.
#   make            make firmware
#   make <board>    that board's firmware.img, e.g. gamesir-cyclone-2-pad
#   make firmware   the firmware of every board but the U2C Bluetooth (which
#                   needs arm-none-eabi-gcc and a stock dump: make
#                   8bitdo-ultimate2c-bluetooth-pad)
#   make all        every target (the U2C Bluetooth and the tests too)
#   make test       the unit tests (each reruns when it or its build changed)
#   make package / flash / update   see below; make dist (make/dist.mk)
#   make help       the targets
#   make clean
#   make padtool-dist   padtool for Windows/Linux (make/padtool.mk)

include make/common.mk
OBJDIR := $(O)
# The HOST_ toolchain (NAME_toolchain := HOST_) is common.mk's HOST_CC, ...
HOST_ARFLAGS := rcs
include make/lib.mk
.DEFAULT_GOAL := firmware

include make/sources.mk
include make/btstack.mk
include make/jieli.mk
include make/telink.mk
include make/telink-lib.mk
include make/yc3121-lib.mk
include make/host-tests.mk

.PHONY: test

RULES += src/core
RULES += src/lib
RULES += tools/jollymaam
RULES += tools
RULES += src/boards/gamesir-cyclone-2
RULES += src/boards/gamesir-nova-2-lite
RULES += src/boards/8bitdo-ultimate2c-wireless
RULES += src/boards/8bitdo-ultimate2c-bluetooth

$(eval $(call generate-rules,$(RULES)))

# Every board's firmware (each rules.mk adds its boards).
.PHONY: firmware

# make package: what is built, as a firmware package in $(O)/firmware (jollymaam
# package); make flash / make update: that onto every device it fits, through
# the vendor loader / our updater (DEVICE=PORT: that one; WAIT=SECONDS for one
# to appear).  make BOARD... flash builds and flashes those boards only.
.PHONY: package flash update
FLASH_GOALS := $(filter-out package flash update,$(MAKECMDGOALS))
package: $(JOLLYMAAM_CLI) $(or $(FLASH_GOALS),firmware)
	$(Q)rm -rf $(O)/firmware
	$(Q)$(JOLLYMAAM_CLI) package $(O) $(O)/firmware
flash update: package
	$(Q)$(JOLLYMAAM_CLI) $(if $(filter flash,$@),oemflash,update) $(if $(DEVICE),--device $(DEVICE)) \
		$(if $(WAIT),--wait $(WAIT)) $(O)/firmware

include make/dist.mk
include make/android.mk
include make/padtool.mk
