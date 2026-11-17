MAKEFLAGS += -rR
.DEFAULT_GOAL := help

V ?= 0
O ?= objs
QUIET := $(if $(filter 0,$(V)),@,)

.PHONY: FORCE all firmware test clean help info
FORCE:
all:

clean:
	rm -rf $(O)

help:
	$(info Help:)
	$(info $(space)$(space)help)
	$(info $(space)$(space)firmware)
	$(info $(space)$(space)test)
	$(info $(space)$(space)all)
	$(info $(space)$(space)clean)
	$(info $(space)$(space)info)
	$(info $(space))
	$(foreach t,$(sort $(HELP_TARGETS)),$(info $(space)$(space)$t))

info:
	$(info Targets:)
	$(foreach t,$(sort $(LIB_ALL_TARGETS)),$(info $(space)$(space)$t))
	$(info Objects:)
	$(foreach o,$(sort $(LIB_ALL_OBJECTS)),$(info $(space)$(space)$o))
	@:

# newline: a literal newline
define newline


endef
empty :=
space := $(lib-empty) $(lib-empty)

lib-default = $(if $(filter undefined default,$(origin $(1))),$(eval $(1) := $(2)))
$(call lib-default,CC,cc)
$(call lib-default,CXX,c++)
$(call lib-default,AR,ar)
$(call lib-default,ARFLAGS,rcs)
$(call lib-default,HOST_CC,cc)
$(call lib-default,HOST_CFLAGS,-O2 -g)
$(call lib-default,HOST_CXX,c++)
$(call lib-default,HOST_CXXFLAGS,-O2 -g)
$(call lib-default,HOST_AR,ar)
$(call lib-default,HOST_ARFLAGS,rcs)

obj_cppflags :=
obj_cflags :=
obj_cxxflags :=

lib-oneline = $(subst $(newline), ,$(1))
lib-dryrun = $(findstring n,$(firstword -$(MAKEFLAGS)))
lib-cmdfile = $(dir $(1)).$(notdir $(1)).cmd
lib-hash := \#
lib-escape = $(subst $(lib-hash),\$(lib-hash),$(subst $$,$$$$,$(1)))
lib-same = $(and $(findstring $(1),$(2)),$(findstring $(2),$(1)))
lib-changed = $(or $(filter-out FORCE,$?),$(if $(wildcard $@),,missing),$(if $(call lib-same,$(strip $(1)),$(strip $(lib_savedcmd_$@))),,cmd),$(2))

# Kbuild style .cmd tracking
define lib-run
$(if $(call lib-changed,$(1),$(3)),$(if $(lib-dryrun),,$(file >$(call lib-cmdfile,$@).tmp,lib_savedcmd_$@ := $(call lib-escape,$(strip $(1)))))$(if $(filter 0,$(V)),@printf '%s\n' '$(2) $@'; )rm -f '$(call lib-cmdfile,$@)'; $(call lib-oneline,$(1)) && mv -f '$(call lib-cmdfile,$@).tmp' '$(call lib-cmdfile,$@)')
endef

lib-target = $(O)/$(1)
lib-objdir = $(O)/objs-$(patsubst ./,,$(dir $(1)))$(notdir $(1))
lib-objpath = $(patsubst /%,_abs/%,$(subst ../,__/,$(patsubst ./%,%,$(1))))
lib-obj = $(call lib-objdir,$(1))/$(call lib-objpath,$(2)).o
lib-objs = $(foreach s,$(2),$(call lib-obj,$(1),$(s)))
lib-objects = $(call lib-objs,$(1),$($(1)_sources))

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
lib-check-toolchain = $(if $(call lib-tc,$(1)),$(if $(filter $(call lib-tc,$(1))%,$(.VARIABLES)),,$(error target $(1): no variable starts with toolchain prefix '$(call lib-tc,$(1))')))

lib-compile.c = $(lib_cc) $(lib_cppflags) $(obj_cppflags) $(lib_cflags) $(obj_cflags) -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@
lib-compile.S = $(lib-compile.c)
lib-compile.cpp = $(lib_cxx) $(lib_cppflags) $(obj_cppflags) $(lib_cxxflags) $(obj_cxxflags) -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@
lib-compile.cc = $(lib-compile.cpp)
lib-compile.cxx = $(lib-compile.cpp)

lib-type-link = $(lib_linker) $(lib_linkflags) -o $@ $(lib_objects) $(lib_linklibs)
lib-type-archive = rm -f $@ && $(lib_ar) $(lib_arflags) $@ $(lib_objects)

lib-defined = $(filter-out undefined,$(origin $(1)))
lib-compile-var = $(or $(if $(call lib-defined,$(1)_compile$(suffix $(2))),$(1)_compile$(suffix $(2))),$(if $(call lib-defined,lib-compile$(suffix $(2))),lib-compile$(suffix $(2))))

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
.PHONY: $(1)
$(1): $(call lib-target,$(1))
all: $(1)
endef

define lib-tracked-rule
$(if $(filter $(1),$(LIB_ALL_OUTPUTS)),$(error duplicate tracked output '$(1)'))
$(eval LIB_ALL_OUTPUTS += $(1))
$(1): FORCE
all: $(1)
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

define lib-include-one
$(eval TARGETS :=)
$(eval default_type := link)
$(eval TRACKED :=)
$(eval self := $(1))
$(eval selfout := $(O)/$(1))
$(eval include $(1)/rules.mk)
$(foreach t,$(TARGETS),$(eval $(if $($(t)_type),,$(t)_type := $(default_type))))
$(foreach t,$(TARGETS),$(eval $(call lib-register-target,$(t),$(1))))
$(foreach t,$(TRACKED),$(eval $(call lib-tracked-rule,$(t))))
endef

%/:
	$(QUIET)mkdir -p '$@'

define generate-rules
$(foreach d,$(1),$(call lib-include-one,$(d)))
$(eval -include $(patsubst %.o,%.d,$(LIB_ALL_OBJECTS)) $(foreach f,$(LIB_ALL_OUTPUTS),$(call lib-cmdfile,$(f))))
endef
