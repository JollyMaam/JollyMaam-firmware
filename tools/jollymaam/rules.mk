# The jollymaam command, with this platform's hooks.
# JOLLYMAAM_PLATFORM: linux, windows (a MinGW HOST_CC) or hidapi (anything
# else: macOS); JOLLYMAAM_STATIC=1 links statically.
HOST_TRIPLE := $(shell $(HOST_CC) -dumpmachine)
JOLLYMAAM_PLATFORM ?= $(if $(findstring mingw,$(HOST_TRIPLE)),windows,$(if $(findstring linux,$(HOST_TRIPLE)),linux,hidapi))
JOLLYMAAM_EXEEXT := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),.exe)
JOLLYMAAM_LIBS := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),-lsetupapi -lhid -lcfgmgr32) \
	$(if $(filter hidapi,$(JOLLYMAAM_PLATFORM)),$(shell pkg-config --cflags --libs hidapi))
JOLLYMAAM_CLI := $(O)/host/jollymaam$(JOLLYMAAM_EXEEXT)

TARGETS += host/jollymaam$(JOLLYMAAM_EXEEXT)
host/jollymaam$(JOLLYMAAM_EXEEXT)_toolchain := HOST_
host/jollymaam$(JOLLYMAAM_EXEEXT)_sources := $(self)/jollymaam.c $(self)/image.c \
	$(self)/hooks_$(if $(filter windows,$(JOLLYMAAM_PLATFORM)),win32,$(JOLLYMAAM_PLATFORM)).c
host/jollymaam$(JOLLYMAAM_EXEEXT)_cflags := -std=c99
host/jollymaam$(JOLLYMAAM_EXEEXT)_deps := $(O)/host/libjollymaam.a
host/jollymaam$(JOLLYMAAM_EXEEXT)_ldflags := $(if $(filter 1,$(JOLLYMAAM_STATIC)),-static $(addprefix -L,$(JOLLYMAAM_LIBC_STATIC))) $(JOLLYMAAM_LDFLAGS)
host/jollymaam$(JOLLYMAAM_EXEEXT)_ldlibs := $(O)/host/libjollymaam.a $(JOLLYMAAM_LIBS)

# An updater and an app above it as one image (pack join; see jollymaam
# help), from the target's jollymaam_join.
lib-type-jollymaam-join = $(JOLLYMAAM_CLI) pack join -o $@ $(jollymaam_join)

.PHONY: jollymaam
jollymaam: $(JOLLYMAAM_CLI)
libjollymaam-test: $(JOLLYMAAM_CLI)
	$(Q)$(JOLLYMAAM_CLI) help > /dev/null

# make install: the command, the library and its headers under PREFIX
# (DESTDIR: a staging prefix).
PREFIX ?= /usr/local
.PHONY: install
install: $(JOLLYMAAM_CLI) $(O)/host/libjollymaam.a
	$(Q)install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/jollymaam/jieli_image
	$(Q)install -m755 $(JOLLYMAAM_CLI) $(DESTDIR)$(PREFIX)/bin/
	$(Q)install -m644 $(O)/host/libjollymaam.a $(DESTDIR)$(PREFIX)/lib/
	$(Q)install -m644 src/lib/*.h $(DESTDIR)$(PREFIX)/include/jollymaam/
	$(Q)install -m644 src/lib/jieli_image/*.h $(DESTDIR)$(PREFIX)/include/jollymaam/jieli_image/

# Every board's firmware as one package (jollymaam package): each product's
# install image (firmware.img) and update.bin, and the Jieli RAM loaders.
# The boards add their firmware.img to its prerequisites (update.bin comes
# with it): they are read after this file.
FIRMWARE_ZIP := $(O)/firmware.zip
TRACKED += $(FIRMWARE_ZIP)
$(FIRMWARE_ZIP): $(JOLLYMAAM_CLI)
	$(call lib-run,$(JOLLYMAAM_CLI) package $(O) $@,PACKAGE)
firmware: $(FIRMWARE_ZIP)
