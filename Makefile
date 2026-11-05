# Built with make/lib.mk: each directory in RULES has a rules.mk with its
# targets; everything builds into $(O) (OBJDIR), a board into $(O)/<board>/.
#   make            every target
#   make <board>    that board's firmware.img, e.g. gamesir-cyclone-2-pad
#   make help       the targets
#   make clean
# So far only the GameSir Cyclone 2 and the jollymaam command are on lib.mk;
# the other products' make/*.mk are not included yet.

include make/common.mk
OBJDIR := $(O)
# The HOST_ toolchain (NAME_toolchain := HOST_) is common.mk's HOST_CC, ...
HOST_ARFLAGS := rcs
include make/lib.mk
.DEFAULT_GOAL := all

include make/btstack.mk
include make/jieli.mk
include make/jieli-lib.mk

RULES += src/core
RULES += src/lib
RULES += tools/jollymaam
RULES += src/boards/gamesir-cyclone-2

$(eval $(call generate-rules,$(RULES)))
