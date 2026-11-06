# 8BitDo Ultimate 2C Wireless: the pad, a Telink TLSR8208 (B80).
#   8bitdo-ultimate2c-wireless-pad          in $(O)/<board>/: update.bin, the
#                                           app's header and the app, above its
#                                           updater; firmware.img, the two, for
#                                           the boot loader
#   8bitdo-ultimate2c-wireless-pad-updater  the pad's updater: part of -pad
#   8bitdo-ultimate2c-wireless-pad-stack    the deepest stack use against the room
#                                           for it (part of -pad-test)
#   8bitdo-ultimate2c-wireless-pad-test, 8bitdo-ultimate2c-wireless-all
# A board's targets, all in $(O)/<board>/: boot.link (the SDK's linker
# script, edited), app.elf (and app.map), app.bin, firmware.bin (with the
# image header); for the pad also firmware.img.
# The boot loader is 0x0000-0x5fff and starts the updater at 0x6000; stock
# keeps data in sectors from 0x73000 (docs/8bitdo-ultimate2c-wireless/),
# which nothing here writes.  The updater has 64 KiB, the app's header is at
# 0x16000 and the app after it; settings and bonds stay at 0x6f000.
# make/telink-lib.mk has the toolchain, the source lists and the types.

U2CW_PAD := 8bitdo-ultimate2c-wireless-pad
U2CW_PAD_DIR := $(O)/$(U2CW_PAD)
U2CW_UPDATER := 8bitdo-ultimate2c-wireless-pad-updater
U2CW_UPDATER_DIR := $(O)/$(U2CW_UPDATER)
U2CW_STAMP := 6a0000005d02

U2CW_LDFLAGS := --wrap=otp_read --wrap=bls_smp_param_saveBondingInfo --wrap=l2cap_att_handler
# Root the replacement explicitly: --defsym alone does not retain it with
# this linker's --gc-sections (the alias would otherwise resolve to zero).
U2CW_LDFLAGS += --wrap=blt_connect --undefined=plat_ll_connect_request
U2CW_LDFLAGS += --defsym=blc_ll_procConnectReq=plat_ll_connect_request
U2CW_LDFLAGS += --wrap=blc_ll_get_connEffectiveMaxTxOctets
U2CW_LDFLAGS += --undefined=plat_ll_push_tx_fifo --defsym=bls_ll_pushTxFifo=plat_ll_push_tx_fifo

# Our files' includes: the board's after the platform's.
U2CW_INCLUDES := $(TELINK_SRC_INCLUDES) -iquote $(self) $(TELINK_INCLUDES) $(BTSTACK_INCLUDES)

# 16 KiB of SRAM: the settings keep slots A-Y and remap slots A-Y with the
# base, 8 rules each; records sized for them and a small staging buffer;
# small log and trace rings.
U2CW_DEFINES :=
U2CW_DEFINES += -DSLOTS_STORED=4
# The CPU at 48 MHz: code runs from flash through a 2 KiB cache, and a pass
# of the control loop's input steps took ~1.8 ms at 24 MHz, which the just
# in time reports must wait out (U2CW_CPU_HZ=24000000 for the old clock).
U2CW_CPU_HZ ?= 48000000
U2CW_DEFINES += -DCLOCK_SYS_CLOCK_HZ=$(U2CW_CPU_HZ)
# U2CW_SLEEP=deep U2CW_LAST_SLOT=analog restores the retained-supply mode.
# U2CW_LAST_SLOT=flash opts into last-slot writes; none never accesses it.
# Long-term idle cuts the board's power by default. Deep sleep deliberately
# keeps the power hold asserted; select it explicitly for current measurements.
U2CW_SLEEP ?= poweroff
U2CW_LAST_SLOT ?= none
ifeq ($(U2CW_SLEEP),deep)
U2CW_DEFINES += -DU2CW_DEEP_SLEEP
else ifneq ($(U2CW_SLEEP),poweroff)
$(error U2CW_SLEEP must be poweroff or deep)
endif
ifeq ($(U2CW_LAST_SLOT),analog)
U2CW_DEFINES += -DU2CW_LAST_SLOT_ANALOG
else ifeq ($(U2CW_LAST_SLOT),flash)
U2CW_DEFINES += -DU2CW_LAST_SLOT_FLASH
else ifeq ($(U2CW_LAST_SLOT),none)
U2CW_DEFINES += -DU2CW_REQUIRE_SLOT_SELECTION
else
$(error U2CW_LAST_SLOT must be none, analog or flash)
endif
# The latency probe (padtool latency) is off: its ring fits only in the
# spare RAM in .data, for test builds (make ... U2CW_PROBE_EDGES=6), which
# power off instead of deep sleep to make the room.
U2CW_PROBE_EDGES ?= 0
U2CW_DEFINES += -DPROBE_EDGES=$(U2CW_PROBE_EDGES)
ifneq ($(U2CW_PROBE_EDGES),0)
U2CW_DEFINES += -DPROBE_IN_DATA -DNO_DEEP_SLEEP
endif
# The sampling profiler (padtool profile) is off too: its ring takes the
# same spare RAM, for test builds (make ... U2CW_PROFILE=1).
U2CW_PROFILE ?= 0
U2CW_PROFILE_RING ?= 28
ifneq ($(U2CW_PROFILE),0)
ifneq ($(U2CW_PROBE_EDGES),0)
$(error U2CW_PROFILE and U2CW_PROBE_EDGES share the spare RAM: one at a time)
endif
U2CW_DEFINES += -DPROF_RING=$(U2CW_PROFILE_RING) -DNO_DEEP_SLEEP
endif
U2CW_DEFINES += -DREMAPS_STORED=4
U2CW_DEFINES += -DREMAP_RULES=8
U2CW_DEFINES += -DRECORD_MAX=152
U2CW_DEFINES += -DSLOT_USB_AUDIO=0
U2CW_DEFINES += -DSTAGE_MAX=256
# The settings store (core/store.c): a sector each, two below the bonds
# (bt_telink.c).
U2CW_DEFINES += -DSTORE_A=0x6f000 -DSTORE_B=0x70000 -DSTORE_BANK=0x1000
U2CW_DEFINES += -DLOG_LINES=2
U2CW_DEFINES += -DLOG_TEXT=48
U2CW_DEFINES += -DTRACE=1
U2CW_DEFINES += -DUSB_RESULTS=64
# Config mode borrows the tool device's command buffer (plat_tool_lend);
# its answers go in pieces of an ATT message (att_telink.h).
U2CW_DEFINES += -DCONFIG_LENT_BUFFER -DCONFIG_REPLY_MAX=144

# --- The pad's updater (src/updater/, docs/firmware/updater.md): where the boot
# loader starts an app (0x6000), up to the app's header at 0x16000 -----------

U2CW_UPDATER_FLAGS := -DBOARD_PID=\"U2C_WIRELESS\" -DAPP_START=0x6000 -DLOG_LINES=16 -DLOG_TEXT=48 \
	$(filter -DU2CW_DEEP_SLEEP -DNO_DEEP_SLEEP,$(U2CW_DEFINES)) $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS) \
	-include $(TELINK_APP_CONFIG) -DUPDATER -DMAIN_AT=0x10000 -DCODE_LIMIT=0x6f000

U2CW_UPDATER_SOURCES :=
U2CW_UPDATER_SOURCES += src/updater/updater.c
U2CW_UPDATER_SOURCES += src/updater/updater_telink.c
U2CW_UPDATER_SOURCES += src/updater/updater_ble_telink.c
U2CW_UPDATER_SOURCES += $(self)/board_pad.c
U2CW_UPDATER_SOURCES += src/platform/telink/platform_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/platform_telink_usb.c
U2CW_UPDATER_SOURCES += src/platform/telink/libc_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/bt_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/rf8_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/att_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/connect_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/tx_telink.c
U2CW_UPDATER_SOURCES += src/platform/telink/rx_telink.c
U2CW_UPDATER_SOURCES += src/core/utils.c
U2CW_UPDATER_SOURCES += src/core/battery.c

TARGETS += $(U2CW_UPDATER)/boot.link
$(U2CW_UPDATER)/boot.link_type := telink-boot-link
$(U2CW_UPDATER)/boot.link_deps := $(TELINK_SDK)/project/tlsr_tc32/B80/boot.link
$(U2CW_UPDATER_DIR)/boot.link: private telink_app_start := 0x6000

TARGETS += $(U2CW_UPDATER)/app.elf
$(U2CW_UPDATER)/app.elf_toolchain := TELINK_B80_
$(U2CW_UPDATER)/app.elf_sources := $(U2CW_UPDATER_SOURCES) $(TELINK_SDK_SOURCES) $(TELINK_BTSTACK_SOURCES)
$(U2CW_UPDATER)/app.elf_cppflags := $(U2CW_UPDATER_FLAGS)
$(U2CW_UPDATER)/app.elf_ldflags := $(U2CW_LDFLAGS) -T $(U2CW_UPDATER_DIR)/boot.link -Map $(U2CW_UPDATER_DIR)/app.map
$(U2CW_UPDATER)/app.elf_deps := $(U2CW_UPDATER_DIR)/boot.link $(TELINK_ARCHIVES)
$(U2CW_UPDATER)/app.elf_order_deps := $(BUILD_ID_H)

$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(U2CW_UPDATER_SOURCES)): obj_cppflags += $(U2CW_INCLUDES)
$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(U2CW_UPDATER_SOURCES)): obj_cflags += $(TELINK_CFLAGS)
$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(TELINK_SDK_SOURCES)): obj_cppflags += $(TELINK_INCLUDES)
$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(TELINK_SDK_C_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS) -w
$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(TELINK_SDK_S_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS)
$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cppflags += $(BTSTACK_RENAMES) $(TELINK_BTSTACK_INCLUDES)
$(call lib-objs,$(U2CW_UPDATER)/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cflags += $(TELINK_CFLAGS)

TARGETS += $(U2CW_UPDATER)/app.bin
$(U2CW_UPDATER)/app.bin_type := telink-bin
$(U2CW_UPDATER)/app.bin_deps := $(U2CW_UPDATER_DIR)/app.elf

TARGETS += $(U2CW_UPDATER)/firmware.bin
$(U2CW_UPDATER)/firmware.bin_type := telink-fw
$(U2CW_UPDATER)/firmware.bin_deps := $(U2CW_UPDATER_DIR)/app.bin $(JOLLYMAAM_CLI)
$(U2CW_UPDATER_DIR)/firmware.bin: private telink_stamp := $(U2CW_STAMP)
$(U2CW_UPDATER_DIR)/firmware.bin: private telink_app_start := 0x6000
$(U2CW_UPDATER_DIR)/firmware.bin: private telink_app_end := 0x16000

.PHONY: $(U2CW_UPDATER)
$(U2CW_UPDATER): $(U2CW_UPDATER_DIR)/firmware.bin

# --- The pad: its header at 0x16000 (0x10000 above the updater), the app 0x120
# after it, up to the settings at 0x6f000 ---------------------------------------

U2CW_PAD_FLAGS := -DBOARD_PID=\"U2C_WIRELESS\" -DAPP_START=0x16120 $(U2CW_DEFINES) $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS) \
	-include $(TELINK_APP_CONFIG)

U2CW_PAD_SOURCES :=
U2CW_PAD_SOURCES += $(self)/board_pad.c
U2CW_PAD_SOURCES += src/platform/telink/platform_telink.c
U2CW_PAD_SOURCES += src/platform/telink/platform_telink_usb.c
U2CW_PAD_SOURCES += src/platform/telink/libc_telink.c
U2CW_PAD_SOURCES += src/platform/telink/store_telink.c
U2CW_PAD_SOURCES += src/core/store.c
U2CW_PAD_SOURCES += src/platform/telink/bt_telink.c
U2CW_PAD_SOURCES += src/platform/telink/rf8_telink.c
U2CW_PAD_SOURCES += src/platform/telink/att_telink.c
U2CW_PAD_SOURCES += src/platform/telink/connect_telink.c
U2CW_PAD_SOURCES += src/platform/telink/tx_telink.c
U2CW_PAD_SOURCES += src/platform/telink/rx_telink.c
U2CW_PAD_SOURCES += src/core/controller_lite.c
U2CW_PAD_SOURCES += src/profiles/profile_hid.c
U2CW_PAD_SOURCES += src/profiles/profile_switch.c
U2CW_PAD_SOURCES += src/profiles/profile_xbox.c
U2CW_PAD_SOURCES += src/profiles/profile_8bitdo.c
U2CW_PAD_SOURCES += src/profiles/profile_playstation.c
U2CW_PAD_SOURCES += src/profiles/profile_config.c
U2CW_PAD_SOURCES += src/core/motion.c
U2CW_PAD_SOURCES += src/core/input.c
U2CW_PAD_SOURCES += src/core/input_calibration.c
U2CW_PAD_SOURCES += src/core/remap.c
U2CW_PAD_SOURCES += src/core/settings.c
U2CW_PAD_SOURCES += src/core/settings_ux.c
U2CW_PAD_SOURCES += src/core/usb.c
U2CW_PAD_SOURCES += src/core/utils.c
U2CW_PAD_SOURCES += src/core/battery.c

TARGETS += $(U2CW_PAD)/boot.link
$(U2CW_PAD)/boot.link_type := telink-boot-link
$(U2CW_PAD)/boot.link_deps := $(TELINK_SDK)/project/tlsr_tc32/B80/boot.link
$(U2CW_PAD_DIR)/boot.link: private telink_app_start := 0x16120

TARGETS += $(U2CW_PAD)/app.elf
$(U2CW_PAD)/app.elf_toolchain := TELINK_B80_
$(U2CW_PAD)/app.elf_sources := $(U2CW_PAD_SOURCES) $(TELINK_SDK_SOURCES) $(TELINK_BTSTACK_SOURCES)
$(U2CW_PAD)/app.elf_cppflags := $(U2CW_PAD_FLAGS)
$(U2CW_PAD)/app.elf_ldflags := $(U2CW_LDFLAGS) -T $(U2CW_PAD_DIR)/boot.link -Map $(U2CW_PAD_DIR)/app.map
$(U2CW_PAD)/app.elf_deps := $(U2CW_PAD_DIR)/boot.link $(TELINK_ARCHIVES)
$(U2CW_PAD)/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,$(U2CW_PAD)/app.elf,$(U2CW_PAD_SOURCES)): obj_cppflags += $(U2CW_INCLUDES)
$(call lib-objs,$(U2CW_PAD)/app.elf,$(U2CW_PAD_SOURCES)): obj_cflags += $(TELINK_CFLAGS)
$(call lib-objs,$(U2CW_PAD)/app.elf,$(TELINK_SDK_SOURCES)): obj_cppflags += $(TELINK_INCLUDES)
$(call lib-objs,$(U2CW_PAD)/app.elf,$(TELINK_SDK_C_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS) -w
$(call lib-objs,$(U2CW_PAD)/app.elf,$(TELINK_SDK_S_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS)
$(call lib-objs,$(U2CW_PAD)/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cppflags += $(BTSTACK_RENAMES) $(TELINK_BTSTACK_INCLUDES)
$(call lib-objs,$(U2CW_PAD)/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cflags += $(TELINK_CFLAGS)

TARGETS += $(U2CW_PAD)/app.bin
$(U2CW_PAD)/app.bin_type := telink-bin
$(U2CW_PAD)/app.bin_deps := $(U2CW_PAD_DIR)/app.elf

TARGETS += $(U2CW_PAD)/firmware.bin
$(U2CW_PAD)/firmware.bin_type := telink-fw
$(U2CW_PAD)/firmware.bin_deps := $(U2CW_PAD_DIR)/app.bin $(JOLLYMAAM_CLI)
$(U2CW_PAD_DIR)/firmware.bin: private telink_stamp := $(U2CW_STAMP)
$(U2CW_PAD_DIR)/firmware.bin: private telink_app_start := 0x16120
$(U2CW_PAD_DIR)/firmware.bin: private telink_app_end := 0x6f000

# Behind its updater: firmware.img, the updater and the app above it, for the
# boot loader (make flash, padtool boot flash, padgui), and update.bin, the
# app's header and the app (padtool update too).
TARGETS += $(U2CW_PAD)/firmware.img
$(U2CW_PAD)/firmware.img_type := jollymaam-join
$(U2CW_PAD)/firmware.img_deps := $(U2CW_UPDATER_DIR)/firmware.bin $(U2CW_PAD_DIR)/firmware.bin $(JOLLYMAAM_CLI)
$(U2CW_PAD_DIR)/firmware.img: private jollymaam_join := --pid U2C_WIRELESS --at 0x10000 --entry 0x16120 \
	--main-out $(U2CW_PAD_DIR)/update.bin $(U2CW_UPDATER_DIR)/firmware.bin $(U2CW_PAD_DIR)/firmware.bin

.PHONY: $(U2CW_PAD)
$(U2CW_PAD): $(U2CW_PAD_DIR)/firmware.img

# --- Tests ---------------------------------------------------------------------

# Unit tests (make/host-tests.mk) with the pad's defines; the settings test
# checks its records against core/store.c's banks.
TARGETS += host/test-$(U2CW_PAD)
host/test-$(U2CW_PAD)_type := host-tests
host/test-$(U2CW_PAD)_toolchain := HOST_
host/test-$(U2CW_PAD)_sources := $(HOST_TESTS)
host/test-$(U2CW_PAD)_cppflags := -DBOARD_PID=\"U2C_WIRELESS\" $(U2CW_DEFINES) $(EXTRA_DEFINES) -DUNITTEST -DSETTINGS_STORE \
	$(SRC_INCLUDES) -iquote $(self)
host/test-$(U2CW_PAD)_compile.c = $(host-test-compile)
$(call lib-objs,host/test-$(U2CW_PAD),$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-$(U2CW_PAD),$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-$(U2CW_PAD),$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

# Our ATT server against BTstack's database code.
TARGETS += host/test-telink-att
host/test-telink-att_toolchain := HOST_
host/test-telink-att_sources := src/platform/telink/att_telink.c $(TELINK_BTSTACK_SOURCES)
host/test-telink-att_cppflags := -DUNITTEST -I src/core -I src/platform/telink $(BTSTACK_INCLUDES)
TRACKED += $(O)/host/telink-att-test.ok
$(O)/host/telink-att-test.ok: $(O)/host/test-telink-att
	$(call lib-run,$< && touch $@,TEST)

# The deepest stack use against the room above .bss, and the interrupts'
# against irq_stk (tools/stackdepth.py).  The pointer calls: the profiles'
# ops (struct profile words 3 begin, 4 end, 5 report, 6 write, 7 host, 8 read), ATT's
# callbacks, the SDK's event, L2CAP and host callbacks.  The settings paths
# that read in or put out a remap are taken only for remap targets, never
# from the slot, calibration and boot ones; a held remap is the only dirty
# one, so writing it reads nothing in.  The store finds its log once, at boot.
U2CW_OPS = $(foreach p,generic_ble xbox_ble switch2,$(1)=@profile_$(p):$(2))
U2CW_STACK_RULES := $(call U2CW_OPS,bt_events,3) $(call U2CW_OPS,bt_events,6)
U2CW_STACK_RULES += $(call U2CW_OPS,start_session,3) $(call U2CW_OPS,report,5)
U2CW_STACK_RULES += $(call U2CW_OPS,host,7) $(call U2CW_OPS,read_value,8)
U2CW_STACK_RULES += bt_events=@profile_config:3 bt_events=@profile_config:4 bt_events=@profile_config:6
U2CW_STACK_RULES += start_config=@profile_config:3 report=@profile_config:5 ctl_main=@profile_config:4
U2CW_STACK_RULES += att_handle_request=read_value,write_value
U2CW_STACK_RULES += blt_event_callback_func=on_connect,on_terminate,on_phy,on_length
U2CW_STACK_RULES += $(foreach f,blt_connect bls_procPendingEvent blt_send_adv bls_ll_security_proc,$(f)=blt_event_callback_func)
U2CW_STACK_RULES += blt_ll_packet_proc=blt_event_callback_func,plat_l2cap_receive
U2CW_STACK_RULES += blc_gap_send_event=on_host
# Through ll_push_tx_fifo_handler, from a stub that looks like a call to itself.
U2CW_STACK_RULES += blc_l2cap_pushData_2_controller=bls_ll_pushTxFifo
U2CW_STACK_RULES += $(foreach f,set_bond_store set_pairing_store set_cal_store renew_identity set_init write_target,\
	$(f)>>release $(f)>>load $(f)>>stored_field.clone.2)
U2CW_STACK_RULES += find_log<<set_init

.PHONY: $(U2CW_PAD)-stack $(U2CW_PAD)-test telink-att-test 8bitdo-ultimate2c-wireless-test
$(U2CW_PAD)-stack: $(U2CW_PAD_DIR)/app.elf
	$(Q)python3 -I tools/stackdepth.py --tools $(TELINK_TOOLS) $< $(foreach r,$(U2CW_STACK_RULES),'$(r)')
telink-att-test: $(O)/host/telink-att-test.ok
$(U2CW_PAD)-test: $(O)/host/test-$(U2CW_PAD) telink-att-test | $(U2CW_PAD)-stack
8bitdo-ultimate2c-wireless-test: $(U2CW_PAD)-test
test: 8bitdo-ultimate2c-wireless-test

.PHONY: 8bitdo-ultimate2c-wireless-all
8bitdo-ultimate2c-wireless-all: $(U2CW_PAD)
firmware: 8bitdo-ultimate2c-wireless-all
