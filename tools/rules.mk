# The hub daemon (tools/padhub.c, tools/padhub.md): the dongle's profiles,
# built for this PC, and its test.
ifneq ($(findstring linux,$(HOST_TRIPLE)),)
PADHUB_SOURCES := $(self)/padhub.c src/profiles/profile_link.c src/profiles/profile_switch.c \
	src/profiles/profile_xbox.c src/profiles/profile_playstation.c src/profiles/profile_hid.c \
	src/core/motion.c src/core/utils.c
PADHUB_CPPFLAGS := -DBOARD_DONGLE $(SRC_INCLUDES) -iquote src/boards/gamesir-cyclone-2

TARGETS += host/padhub
host/padhub_toolchain := HOST_
host/padhub_sources := $(PADHUB_SOURCES)
host/padhub_cppflags := $(PADHUB_CPPFLAGS)

# test_padhub.c includes padhub.c.
TARGETS += host/test-padhub
host/test-padhub_toolchain := HOST_
host/test-padhub_sources := $(self)/test_padhub.c $(filter-out $(self)/padhub.c,$(PADHUB_SOURCES))
host/test-padhub_cppflags := $(PADHUB_CPPFLAGS)

TRACKED += $(O)/host/padhub-test.ok
$(O)/host/padhub-test.ok: $(O)/host/test-padhub
	$(call lib-run,$< && touch $@,TEST)

.PHONY: padhub padhub-test
padhub: $(O)/host/padhub
padhub-test: $(O)/host/padhub-test.ok
test: padhub-test
endif
