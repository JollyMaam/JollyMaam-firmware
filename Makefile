# Each product's targets are in its make/<product>.mk.  Every board builds
# into $(O)/<target>/; host tools (built with HOST_CC) into $(O)/host/.
#   make            every product
#   make test       every product's unit tests
#   make flash-all  every pad and dongle running our firmware, each with this
#                   tree's build for its PID, by its USB port
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

.PHONY: all test flash-all
all: gamesir-cyclone-2-all gamesir-nova-2-lite-all 8bitdo-ultimate2c-wireless-all
test: gamesir-cyclone-2-test gamesir-nova-2-lite-test 8bitdo-ultimate2c-wireless-test

# Each image PID's flash target (IDENTIFY's PID, as jollymaam scan --ours
# lists them).
FLASH_TARGET.GS_C2_ADC_DEVICE := gamesir-cyclone-2-pad
FLASH_TARGET.GS_C2_Dongle := gamesir-cyclone-2-dongle
FLASH_TARGET.AC695X_TRANS := gamesir-nova-2-lite-pad
FLASH_TARGET.AC632N_TRANS := gamesir-nova-2-lite-dongle
FLASH_TARGET.U2C_WIRELESS := 8bitdo-ultimate2c-wireless-pad
FLASH_TARGET.U2C_BLUETOOTH := 8bitdo-ultimate2c-bluetooth-pad
rparen := )
FLASH_PIDS := $(patsubst FLASH_TARGET.%,%,$(filter FLASH_TARGET.%,$(.VARIABLES)))

# One at a time, each by its port; a failure is reported and the rest go on.
flash-all: $(JOLLYMAAM_CLI)
	@$(JOLLYMAAM_CLI) scan --ours > $(O)/flash-all.list; failed=; \
	if [ ! -s $(O)/flash-all.list ]; then echo "flash-all: no device runs our firmware"; exit 1; fi; \
	while read port pid; do \
	  case $$pid in \
	  $(foreach p,$(FLASH_PIDS),$(p)$(rparen) target=$(FLASH_TARGET.$(p));;) \
	  *) echo "== $$pid at $$port: no target, left alone"; continue;; \
	  esac; \
	  echo "== $$pid at $$port: make $$target-flash"; \
	  $(MAKE) --no-print-directory $$target-flash DEVICE=$$port < /dev/null || failed="$$failed $$pid@$$port"; \
	done < $(O)/flash-all.list; \
	if [ -n "$$failed" ]; then echo "flash-all: failed:$$failed"; exit 1; fi
