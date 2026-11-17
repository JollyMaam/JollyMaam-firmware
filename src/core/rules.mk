# build_id.h: BUILD_ID (git describe), for usb.c and updater.c. It is
# rewritten only when the ID changes, so a new commit rebuilds those two
# files, not every object. A board adds $(BUILD_ID_CPPFLAGS) to its flags
# and $(BUILD_ID_H) to its order-only deps.
BUILD_ID_H := $(selfout)/build_id.h
BUILD_ID_CPPFLAGS := -DHAVE_BUILD_ID_H -iquote $(selfout)
TRACKED += $(BUILD_ID_H)
$(BUILD_ID_H): | $(selfout)/
	$(call lib-run,printf '#define BUILD_ID "%s"\n' '$(BUILD_ID)' > $@,GEN)
