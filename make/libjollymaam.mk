# libjollymaam and the jollymaam command (tools/jollymaam/).
#   jollymaam           $(O)/host/jollymaam: the command, with this platform's hooks
#   libjollymaam        $(O)/host/libjollymaam.a: the library alone (no OS calls)
#   libjollymaam-test   the library's unit tests and simulations (part of make test)
#   install             the command, the library and its headers under PREFIX
# JOLLYMAAM_PLATFORM: linux, windows (a MinGW HOST_CC) or hidapi (anything
# else: macOS); JOLLYMAAM_STATIC=1 links statically.
HOST_TRIPLE := $(shell $(HOST_CC) -dumpmachine)
JOLLYMAAM_PLATFORM ?= $(if $(findstring mingw,$(HOST_TRIPLE)),windows,$(if $(findstring linux,$(HOST_TRIPLE)),linux,hidapi))
JOLLYMAAM_EXEEXT := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),.exe)
JOLLYMAAM_HOOKS := tools/jollymaam/hooks_$(if $(filter windows,$(JOLLYMAAM_PLATFORM)),win32,$(JOLLYMAAM_PLATFORM)).c
JOLLYMAAM_LIBS := $(if $(filter windows,$(JOLLYMAAM_PLATFORM)),-lsetupapi -lhid -lcfgmgr32) \
	$(if $(filter hidapi,$(JOLLYMAAM_PLATFORM)),$$(pkg-config --cflags --libs hidapi))
JOLLYMAAM_LINK_FLAGS = $(if $(filter 1,$(JOLLYMAAM_STATIC)),-static $(addprefix -L,$(JOLLYMAAM_LIBC_STATIC))) $(JOLLYMAAM_LDFLAGS)
JOLLYMAAM_CLI := $(O)/host/jollymaam$(JOLLYMAAM_EXEEXT)
JOLLYMAAM_HEADERS := $(wildcard src/lib/*.h src/lib/jieli_image/*.h)
JOLLYMAAM_SOURCES := $(wildcard src/lib/*.c src/lib/jieli_image/*.c)
JOLLYMAAM_OBJECTS := $(JOLLYMAAM_SOURCES:src/lib/%.c=$(O)/host/libjollymaam/%.o)
JOLLYMAAM_APP := tools/jollymaam/jollymaam.c tools/jollymaam/image.c $(JOLLYMAAM_HOOKS)

# Rebuild when the compiler or its flags change in the same output directory.
$(O)/host/.jollymaam-config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(HOST_CC)' '$(HOST_CFLAGS)' '$(JOLLYMAAM_PLATFORM)' '$(JOLLYMAAM_STATIC)' '$(JOLLYMAAM_LDFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

.PHONY: libjollymaam jollymaam
libjollymaam: $(O)/host/libjollymaam.a
jollymaam: $(JOLLYMAAM_CLI)
$(O)/host/libjollymaam.a: $(JOLLYMAAM_OBJECTS)
	$(Q)rm -f $@
	$(Q)$(HOST_AR) rcs $@ $^
$(O)/host/libjollymaam/%.o: src/lib/%.c $(JOLLYMAAM_HEADERS) $(O)/host/.jollymaam-config
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -c $< -o $@
$(JOLLYMAAM_CLI): $(JOLLYMAAM_APP) $(wildcard tools/jollymaam/*.h) $(O)/host/libjollymaam.a
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $(JOLLYMAAM_APP) $(O)/host/libjollymaam.a $(JOLLYMAAM_LINK_FLAGS) $(JOLLYMAAM_LIBS) -o $@

# Every library module with a UNITTEST block, and the simulations.
JOLLYMAAM_UNIT := $(basename $(notdir $(shell grep -lE '^#ifdef UNITTEST' $(JOLLYMAAM_SOURCES))))
JOLLYMAAM_SIMS := upgrade jieli device
JOLLYMAAM_TESTS := $(addprefix $(O)/host/test-jollymaam-,$(JOLLYMAAM_UNIT) $(JOLLYMAAM_SIMS))
$(O)/host/test-jollymaam-%: src/lib/%.c $(O)/host/libjollymaam.a
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
$(O)/host/test-jollymaam-%: src/lib/jieli_image/%.c $(O)/host/libjollymaam.a
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 -DUNITTEST $< $(O)/host/libjollymaam.a -o $@
$(addprefix $(O)/host/test-jollymaam-,$(JOLLYMAAM_SIMS)): $(O)/host/test-jollymaam-%: tools/jollymaam/test_jollymaam_%.c $(O)/host/libjollymaam.a
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -std=c99 $< $(O)/host/libjollymaam.a -o $@
.PHONY: libjollymaam-test
libjollymaam-test: $(JOLLYMAAM_TESTS) $(JOLLYMAAM_CLI)
	$(Q)for t in $(JOLLYMAAM_TESTS); do $(HOST_RUNNER) $$t > $$t.out 2>&1 || { cat $$t.out; exit 1; }; done
	$(Q)$(JOLLYMAAM_CLI) help > /dev/null
test: libjollymaam-test

PREFIX ?= /usr/local
.PHONY: install
install: $(JOLLYMAAM_CLI) $(O)/host/libjollymaam.a
	$(Q)install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/jollymaam/jieli_image
	$(Q)install -m755 $(JOLLYMAAM_CLI) $(DESTDIR)$(PREFIX)/bin/
	$(Q)install -m644 $(O)/host/libjollymaam.a $(DESTDIR)$(PREFIX)/lib/
	$(Q)install -m644 src/lib/*.h $(DESTDIR)$(PREFIX)/include/jollymaam/
	$(Q)install -m644 src/lib/jieli_image/*.h $(DESTDIR)$(PREFIX)/include/jollymaam/jieli_image/
