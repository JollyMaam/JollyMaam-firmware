# The jollymaam command, with this platform's hooks.
# JOLLYMAAM_PLATFORM: linux, windows (a MinGW HOST_CC) or hidapi (anything
# else: macOS); JOLLYMAAM_STATIC=1 links statically.
HOST_TRIPLE := $(shell $(HOST_CC) -dumpmachine)
JOLLYMAAM_PLATFORM ?= $(if $(findstring mingw,$(HOST_TRIPLE)),windows,$(if $(findstring linux,$(HOST_TRIPLE)),linux,hidapi))
JOLLYMAAM_EXEEXT := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),.exe)
JOLLYMAAM_LIBS := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),-lsetupapi -lhid -lcfgmgr32) \
	$(if $(filter hidapi,$(JOLLYMAAM_PLATFORM)),$(shell pkg-config --cflags --libs hidapi))
JOLLYMAAM_CLI := $(OBJDIR)/host/jollymaam$(JOLLYMAAM_EXEEXT)

TARGETS += host/jollymaam$(JOLLYMAAM_EXEEXT)
host/jollymaam$(JOLLYMAAM_EXEEXT)_toolchain := HOST_
host/jollymaam$(JOLLYMAAM_EXEEXT)_sources := $(self)/jollymaam.c $(self)/image.c \
	$(self)/hooks_$(if $(filter windows,$(JOLLYMAAM_PLATFORM)),win32,$(JOLLYMAAM_PLATFORM)).c
host/jollymaam$(JOLLYMAAM_EXEEXT)_cflags := -std=c99
host/jollymaam$(JOLLYMAAM_EXEEXT)_deps := $(OBJDIR)/host/libjollymaam.a
host/jollymaam$(JOLLYMAAM_EXEEXT)_ldflags := $(if $(filter 1,$(JOLLYMAAM_STATIC)),-static $(addprefix -L,$(JOLLYMAAM_LIBC_STATIC))) $(JOLLYMAAM_LDFLAGS)
host/jollymaam$(JOLLYMAAM_EXEEXT)_ldlibs := $(OBJDIR)/host/libjollymaam.a $(JOLLYMAAM_LIBS)
