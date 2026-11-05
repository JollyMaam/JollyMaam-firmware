# Non-recursive GNU Make rules library (GNU Make >= 4.0).
# Include this file, then: $(eval $(call generate-rules,dir1 dir2))
# Each dir provides rules.mk with TARGETS and target properties.
#
# Paths: every path (sources, deps, generated files) is relative to the top
# directory, where make runs. In rules.mk, $(self) is that rules.mk's
# directory and $(selfout) is $(OBJDIR)/$(self), for files it generates:
# NAME_sources := $(self)/main.c common/util.c $(selfout)/gen.c
#
# Targets: NAME is built as $(OBJDIR)/NAME; names are unique and may contain
# "/" (board/app.elf). NAME_type picks the command that makes it: link
# (default; programs, and shared libraries with -shared in NAME_ldflags and
# -fPIC in NAME_cflags; a per-rules.mk default_type := changes the default),
# archive, or your own lib-type-TYPE. Properties: NAME_sources, NAME_deps,
# NAME_order_deps, NAME_type, NAME_toolchain (prefix: HOST uses HOSTCC, ...),
# NAME_cc, NAME_cxx, NAME_ar, NAME_link, NAME_cppflags, NAME_cflags,
# NAME_cxxflags, NAME_ldflags, NAME_ldlibs. NAME_order_deps are built before
# the target and before each of its objects (generated headers).
#
# Objects: each source becomes $(OBJDIR)/DIR/.FILE/SOURCE.o, in a hidden
# directory next to the output (objs/board/.app.elf/src/main.c.o). The
# extension stays (startup.c.o, startup.S.o); "../" becomes "__/" and a
# leading "/" becomes "_abs/". A source is compiled by the command in
# NAME_compile.EXT if the target defines one, else lib-compile.EXT (gcc
# style, below). A source with neither is an error.
#
# Per-object flags are ordinary target-specific variables on the object:
#   $(call lib-objs,NAME,SOURCES): obj_cppflags += -DX=1
# obj_cppflags, obj_cflags and obj_cxxflags come after the target's flags.
# A += value is read at compile time (no $(self) in it); := reads it now.
#
# Change tracking: each output's hidden DIR/.FILE.cmd records the command
# that built it; a changed command rebuilds, and up-to-date outputs cost no
# shell (see lib-run). Plain rules get the same by listing their output in
# TRACKED and using $(call lib-run,COMMAND,LABEL) as the recipe.

# No built-in rules or variables (CC, AR, ARFLAGS, LD, RM, ... are gone):
# lib.mk owns the toolchain defaults below. Make removes the built-ins only
# after reading the makefiles, so while they are read CC still has Make's
# default "cc": set toolchain variables with := or =, not ?=, before
# including lib.mk. The environment and the command line still win.
MAKEFLAGS += -rR

.DEFAULT_GOAL := default
.PHONY: default help all clean FORCE
FORCE:
default: help
all:
clean:

V ?= 0
OBJDIR ?= objs
QUIET := $(if $(filter 0,$(V)),@,)

# Set $(1) to $(2) unless it was set by a makefile, the environment or the
# command line (Make's own defaults do not count).
lib-default = $(if $(filter undefined default,$(origin $(1))),$(eval $(1) := $(2)))
$(call lib-default,CC,cc)
$(call lib-default,CXX,c++)
$(call lib-default,AR,ar)
$(call lib-default,ARFLAGS,rcs)
$(call lib-default,HOSTCC,cc)
$(call lib-default,HOSTCFLAGS,-O2 -g)
$(call lib-default,HOSTCXX,c++)
$(call lib-default,HOSTCXXFLAGS,-O2 -g)
$(call lib-default,HOSTAR,ar)
$(call lib-default,HOSTARFLAGS,rcs)

# Per-object flags, empty unless a target-specific variable sets them. Make
# reads a target-specific += when the object is compiled, like a recipe, so
# its value must not use $(self); use := to read a value right away.
obj_cppflags :=
obj_cflags :=
obj_cxxflags :=

# --- Change tracking ---------------------------------------------------------
# Kbuild style: each output's DIR/.FILE.cmd (lib-cmdfile) holds the command
# that built it as a Make assignment and is included at parse time. A recipe
# expands to nothing (no shell runs) unless a prerequisite is newer ($? minus
# FORCE), the output is missing, or the command differs. An empty recipe
# leaves the target "not remade", so dependents are not rebuilt in a cascade.
# The new .cmd is written by $(file) and published only after success; the
# old one is removed first so a failed command always reruns next time.
# The command runs as one shell line: newlines (a multi-line define) become
# spaces, other whitespace is kept (quoted spaces matter). Comparing with the
# saved command ignores whitespace. With make -n nothing is written.
# lib-run args: command, label, extra rebuild reason (non-empty forces).
define lib-nl


endef
lib-oneline = $(subst $(lib-nl), ,$(1))
lib-dryrun = $(findstring n,$(firstword -$(MAKEFLAGS)))
lib-cmdfile = $(dir $(1)).$(notdir $(1)).cmd
lib-hash := \#
lib-escape = $(subst $(lib-hash),\$(lib-hash),$(subst $$,$$$$,$(1)))
lib-same = $(and $(findstring $(1),$(2)),$(findstring $(2),$(1)))
lib-changed = $(or $(filter-out FORCE,$?),$(if $(wildcard $@),,missing),$(if $(call lib-same,$(strip $(1)),$(strip $(lib_savedcmd_$@))),,cmd),$(2))
define lib-run
$(if $(call lib-changed,$(1),$(3)),$(if $(lib-dryrun),,$(file >$(call lib-cmdfile,$@).tmp,lib_savedcmd_$@ := $(call lib-escape,$(strip $(1)))))$(if $(filter 0,$(V)),@printf '%s\n' '$(2) $@'; )rm -f '$(call lib-cmdfile,$@)'; $(call lib-oneline,$(1)) && mv -f '$(call lib-cmdfile,$@).tmp' '$(call lib-cmdfile,$@)')
endef

# --- Paths ---------------------------------------------------------------------
lib-target = $(OBJDIR)/$(1)
lib-objdir = $(OBJDIR)/$(patsubst ./,,$(dir $(1))).$(notdir $(1))
lib-objpath = $(patsubst /%,_abs/%,$(subst ../,__/,$(patsubst ./%,%,$(1))))
lib-obj = $(call lib-objdir,$(1))/$(call lib-objpath,$(2)).o
# Public: the objects target $(1) builds from sources $(2).
lib-objs = $(foreach s,$(2),$(call lib-obj,$(1),$(s)))
lib-objects = $(call lib-objs,$(1),$($(1)_sources))

# --- Tools and flags of a target -----------------------------------------------
# NAME_toolchain is a prefix for the toolchain variables: with HOST the
# target uses HOSTCC, HOSTCFLAGS, ...; with none, CC, CFLAGS, ... A tool set
# on the target (NAME_cc, ...) replaces the toolchain's; target flags are
# added after the toolchain's. The linker is NAME_link, else PREFIXLINK, else
# the C++ compiler if there are C++ sources, else the C compiler.
lib-tc = $($(1)_toolchain)
lib-cc = $(or $($(1)_cc),$($(call lib-tc,$(1))CC))
lib-cxx = $(or $($(1)_cxx),$($(call lib-tc,$(1))CXX))
lib-ar = $(or $($(1)_ar),$($(call lib-tc,$(1))AR))
lib-linker = $(or $($(1)_link),$($(call lib-tc,$(1))LINK),$(if $(filter %.cc %.cpp %.cxx,$($(1)_sources)),$(call lib-cxx,$(1)),$(call lib-cc,$(1))))
lib-cppflags = $($(call lib-tc,$(1))CPPFLAGS) $($(1)_cppflags) -I$(2)
lib-cflags = $($(call lib-tc,$(1))CFLAGS) $($(1)_cflags)
lib-cxxflags = $($(call lib-tc,$(1))CXXFLAGS) $($(1)_cxxflags)
lib-ldflags = $($(call lib-tc,$(1))LDFLAGS) $($(1)_ldflags)
lib-ldlibs = $($(call lib-tc,$(1))LDLIBS) $($(1)_ldlibs)
lib-arflags = $($(call lib-tc,$(1))ARFLAGS)
# A toolchain prefix that no variable starts with is a typo.
lib-check-toolchain = $(if $(call lib-tc,$(1)),$(if $(filter $(call lib-tc,$(1))%,$(.VARIABLES)),,$(error target $(1): no variable starts with toolchain prefix '$(call lib-tc,$(1))')))

# --- Commands --------------------------------------------------------------------
# All are expanded when the recipe runs, with $@, $<, $^ set. Override one for
# every target by redefining it (after including lib.mk), or for one target
# with NAME_compile.EXT / a custom NAME_type. A command must write $@ and fail
# with a nonzero status.
#
# Compiling: lib_cc, lib_cxx, lib_cppflags, lib_cflags, lib_cxxflags (the
# target's) and obj_cppflags, obj_cflags, obj_cxxflags (the object's). A
# compiler without -MMD simply leaves those options out; then header changes
# are not seen, but everything else still works.
lib-compile.c = $(lib_cc) $(lib_cppflags) $(obj_cppflags) $(lib_cflags) $(obj_cflags) -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@
lib-compile.S = $(lib-compile.c)
lib-compile.cpp = $(lib_cxx) $(lib_cppflags) $(obj_cppflags) $(lib_cxxflags) $(obj_cxxflags) -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@
lib-compile.cc = $(lib-compile.cpp)
lib-compile.cxx = $(lib-compile.cpp)

# Linking (target types): lib_objects (the target's own objects, never all
# prerequisites), lib_deps (NAME_deps, not linker inputs by themselves),
# lib_linker, lib_linkflags, lib_linklibs, lib_ar, lib_arflags.
# For example: lib-type-objcopy = $(OBJCOPY) -O binary $(firstword $(lib_deps)) $@
# archive starts with rm because "ar r" adds to an existing archive and would
# keep members whose sources were removed.
lib-type-link = $(lib_linker) $(lib_linkflags) -o $@ $(lib_objects) $(lib_linklibs)
lib-type-archive = rm -f $@ && $(lib_ar) $(lib_arflags) $@ $(lib_objects)

# The command variable for target $(1)'s source $(2): its own, the default,
# or nothing.
lib-defined = $(filter-out undefined,$(origin $(1)))
lib-compile-var = $(or $(if $(call lib-defined,$(1)_compile$(suffix $(2))),$(1)_compile$(suffix $(2))),$(if $(call lib-defined,lib-compile$(suffix $(2))),lib-compile$(suffix $(2))))

# --- Rules ---------------------------------------------------------------------
# Each object receives private target-specific settings. "private" stops the
# settings from leaking into prerequisites (especially generated sources).
# The settings are written as references ($$(call ...)), not values, so the
# text given to $(eval) never contains flag values: a value is read once,
# like in any makefile (\# stays #, $$ is one $). := still takes the values
# now, right after this rules.mk was read.
# lib-object-rule args: target, rules.mk directory, source, command variable.
define lib-object-rule
$(call lib-obj,$(1),$(3)): private lib_cc := $$(call lib-cc,$(1))
$(call lib-obj,$(1),$(3)): private lib_cxx := $$(call lib-cxx,$(1))
$(call lib-obj,$(1),$(3)): private lib_cppflags := $$(call lib-cppflags,$(1),$(2))
$(call lib-obj,$(1),$(3)): private lib_cflags := $$(call lib-cflags,$(1))
$(call lib-obj,$(1),$(3)): private lib_cxxflags := $$(call lib-cxxflags,$(1))
$(call lib-obj,$(1),$(3)): $(3) FORCE | $(dir $(call lib-obj,$(1),$(3))) $($(1)_order_deps)
	$$(call lib-run,$$($(4)),CC)
endef

define lib-target-rule
$(call lib-target,$(1)): private lib_type := $$($(1)_type)
$(call lib-target,$(1)): private lib_deps := $$($(1)_deps)
$(call lib-target,$(1)): private lib_linker := $$(call lib-linker,$(1))
$(call lib-target,$(1)): private lib_linkflags := $$(call lib-ldflags,$(1))
$(call lib-target,$(1)): private lib_linklibs := $$(call lib-ldlibs,$(1))
$(call lib-target,$(1)): private lib_ar := $$(call lib-ar,$(1))
$(call lib-target,$(1)): private lib_arflags := $$(call lib-arflags,$(1))
$(call lib-target,$(1)): private lib_objects := $$(call lib-objects,$(1))
$(call lib-target,$(1)): $(call lib-objects,$(1)) $($(1)_deps) FORCE | $(dir $(call lib-target,$(1))) $($(1)_order_deps)
	$$(if $$(strip $$(lib-type-$$(lib_type))),$$(call lib-run,$$(lib-type-$$(lib_type)),$$(lib_type)),$$(error Unknown target type "$$(lib_type)" for $$@))
.PHONY: $(1) clean_$(1)
$(1): $(call lib-target,$(1))
all: $(1)
clean: clean_$(1)
clean_$(1):
	$$(QUIET)rm -rf $(call lib-objdir,$(1)) $(call lib-target,$(1)) $(call lib-cmdfile,$(call lib-target,$(1))) $(call lib-cmdfile,$(call lib-target,$(1))).tmp
endef

# Plain rules written in rules.mk get change tracking by listing their output
# in TRACKED and using $(call lib-run,COMMAND,LABEL) as the recipe. This adds
# FORCE, includes the output's .cmd, builds it with all and makes clean
# remove it.
define lib-tracked-rule
$(if $(filter $(1),$(LIB_ALL_OUTPUTS)),$(error duplicate tracked output '$(1)'))
$(eval LIB_ALL_OUTPUTS += $(1))
$(1): FORCE
all: $(1)
.PHONY: clean_tracked clean_tracked_$(1)
clean: clean_tracked
clean_tracked: clean_tracked_$(1)
clean_tracked_$(1):
	$$(QUIET)rm -f '$(1)' '$(call lib-cmdfile,$(1))' '$(call lib-cmdfile,$(1)).tmp'
endef

define lib-register-target
$(if $(filter $(1),$(LIB_ALL_TARGETS)),$(error duplicate target '$(1)' in $(2)))
$(eval LIB_ALL_TARGETS += $(1))
$(call lib-check-toolchain,$(1))
$(eval LIB_ALL_OBJECTS += $(call lib-objects,$(1)))
$(eval LIB_ALL_OUTPUTS += $(call lib-objects,$(1)) $(call lib-target,$(1)))
$(foreach s,$($(1)_sources),$(if $(call lib-compile-var,$(1),$(s)),$(eval $(call lib-object-rule,$(1),$(2),$(s),$(call lib-compile-var,$(1),$(s)))),$(error no lib-compile$(suffix $(s)) or $(1)_compile$(suffix $(s)) for source '$(s)')))
$(eval $(call lib-target-rule,$(1),$(2)))
endef

define lib-subdir-alias
.PHONY: all_$(1) clean_all_$(1)
all_$(1): $(2)
clean_all_$(1): $(addprefix clean_,$(2))
endef

# Input collections are scoped to each included rules.mk. $(self) is its
# directory and $(selfout) the same directory under $(OBJDIR), for files it
# generates; both hold only while it is read (a recipe uses $@, $<, ...).
define lib-include-one
$(eval TARGETS :=)
$(eval default_type := link)
$(eval TRACKED :=)
$(eval self := $(1))
$(eval selfout := $(OBJDIR)/$(1))
$(eval include $(1)/rules.mk)
$(foreach t,$(TARGETS),$(eval $(if $($(t)_type),,$(t)_type := $(default_type))))
$(foreach t,$(TARGETS),$(eval $(call lib-register-target,$(t),$(1))))
$(foreach t,$(TRACKED),$(eval $(call lib-tracked-rule,$(t))))
$(eval $(call lib-subdir-alias,$(1),$(TARGETS)))
endef

# Don't create explicit directory rules with FORCE; directory timestamps
# must not force recompile. Make's implicit rule handles nested paths.
%/:
	$(QUIET)mkdir -p '$@'

.PHONY: help info
help:
	@echo 'Targets: all clean info and individual target names'
	@echo 'Built targets: $(LIB_ALL_TARGETS)'
info:
	@echo 'Targets: $(LIB_ALL_TARGETS)'
	@echo 'Objects: $(LIB_ALL_OBJECTS)'

# Suppress dependency and .cmd inclusion while cleaning (avoids generating
# missing included files and keeps clean working with missing source files).
define generate-rules
$(foreach d,$(1),$(call lib-include-one,$(d)))
$(if $(filter clean clean_%,$(MAKECMDGOALS)),,$(eval -include $(patsubst %.o,%.d,$(LIB_ALL_OBJECTS)) $(foreach f,$(LIB_ALL_OUTPUTS),$(call lib-cmdfile,$(f)))))
endef
