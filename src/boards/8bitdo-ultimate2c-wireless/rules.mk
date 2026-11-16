HELP_TARGETS += u2cw
$(FIRMWARE_ZIP): $(O)/u2cw/pad/firmware.img

firmware: u2cw
test: u2cw-test

u2cw: $(O)/u2cw/pad/firmware.img
telink-att-test: $(O)/host/telink-att-test.ok
u2cw/pad-test: $(O)/host/test-u2cw/pad telink-att-test | u2cw/pad-stackcheck
u2cw-test: u2cw/pad-test u2cw/pad-stackcheck

.PHONY: u2cw u2cw-test u2cw/pad-test u2cw/pad-stackcheck telink-att-test

U2CW_LDFLAGS := --wrap=otp_read --wrap=bls_smp_param_saveBondingInfo --wrap=l2cap_att_handler
U2CW_LDFLAGS += --wrap=blt_connect --undefined=plat_ll_connect_request
U2CW_LDFLAGS += --defsym=blc_ll_procConnectReq=plat_ll_connect_request
U2CW_LDFLAGS += --wrap=blc_ll_get_connEffectiveMaxTxOctets
U2CW_LDFLAGS += --undefined=plat_ll_push_tx_fifo --defsym=bls_ll_pushTxFifo=plat_ll_push_tx_fifo

U2CW_INCLUDES := $(TELINK_SRC_INCLUDES) -iquote src/boards/8bitdo-ultimate2c-wireless $(TELINK_INCLUDES) $(BTSTACK_INCLUDES)

# 16 KiB of SRAM: records and rings sized small.
U2CW_CPU_HZ := 48000000
U2CW_DEFINES := -DSLOTS_STORED=4 -DCLOCK_SYS_CLOCK_HZ=$(U2CW_CPU_HZ)
# U2CW_SLEEP=poweroff|deep (deep keeps the power hold asserted),
# U2CW_LAST_SLOT=none|analog|flash.
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
# Test builds: U2CW_PROBE_EDGES=6 (padtool latency) or U2CW_PROFILE=1
# (padtool profile); both use the spare RAM, one at a time.
U2CW_PROBE_EDGES ?= 0
U2CW_DEFINES += -DPROBE_EDGES=$(U2CW_PROBE_EDGES)
ifneq ($(U2CW_PROBE_EDGES),0)
U2CW_DEFINES += -DPROBE_IN_DATA -DNO_DEEP_SLEEP
endif
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
# The quick macro occupies padding before the instruction cache.
U2CW_DEFINES += -DQUICK_MACRO_BYTES=144 -DQUICK_PADDING_BUFFER
U2CW_DEFINES += -DRECORD_MAX=152
U2CW_DEFINES += -DREMAP_MACRO_BYTES=146
# No RAM for the tool's PRESS (pad.h).
U2CW_DEFINES += -DTOOL_PRESS=0
U2CW_DEFINES += -DSLOT_USB_AUDIO=0
U2CW_DEFINES += -DLEDS_MONO=1
U2CW_DEFINES += -DSTAGE_MAX=256
U2CW_DEFINES += -DSTORE_A=0x6f000 -DSTORE_B=0x70000 -DSTORE_BANK=0x1000
U2CW_DEFINES += -DLOG_LINES=2
U2CW_DEFINES += -DLOG_TEXT=48
U2CW_DEFINES += -DTRACE=1
U2CW_DEFINES += -DUSB_RESULTS=64
U2CW_DEFINES += -DCONFIG_LENT_BUFFER -DCONFIG_REPLY_MAX=144

# --- Updater: 0x6000 (where the boot loader starts), up to the app's header at 0x16000

U2CW_UPDATER_FLAGS := -DBOARD_PID=\"U2C_WIRELESS\" -DAPP_START=0x6000 -DLOG_LINES=16 -DLOG_TEXT=48 \
	$(filter -DU2CW_DEEP_SLEEP -DNO_DEEP_SLEEP,$(U2CW_DEFINES)) $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS) \
	-include $(TELINK_APP_CONFIG) -DUPDATER -DMAIN_AT=0x10000 -DCODE_LIMIT=0x6f000

U2CW_UPDATER_SOURCES :=
U2CW_UPDATER_SOURCES += src/updater/updater.c
U2CW_UPDATER_SOURCES += src/updater/updater_telink.c
U2CW_UPDATER_SOURCES += src/updater/updater_ble_telink.c
U2CW_UPDATER_SOURCES += src/boards/8bitdo-ultimate2c-wireless/board_pad.c
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

TARGETS += u2cw/updater/boot.link
u2cw/updater/boot.link_type := telink-boot-link
u2cw/updater/boot.link_deps := $(TELINK_SDK)/project/tlsr_tc32/B80/boot.link
$(O)/u2cw/updater/boot.link: private telink_app_start := 0x6000

TARGETS += u2cw/updater/app.elf
u2cw/updater/app.elf_toolchain := TELINK_B80_
u2cw/updater/app.elf_sources := $(U2CW_UPDATER_SOURCES) $(TELINK_SDK_SOURCES) $(TELINK_BTSTACK_SOURCES)
u2cw/updater/app.elf_cppflags := $(U2CW_UPDATER_FLAGS)
u2cw/updater/app.elf_ldflags := $(U2CW_LDFLAGS) -T $(O)/u2cw/updater/boot.link -Map $(O)/u2cw/updater/app.map
u2cw/updater/app.elf_deps := $(O)/u2cw/updater/boot.link $(TELINK_ARCHIVES)
u2cw/updater/app.elf_order_deps := $(BUILD_ID_H)

$(call lib-objs,u2cw/updater/app.elf,$(U2CW_UPDATER_SOURCES)): obj_cppflags += $(U2CW_INCLUDES)
$(call lib-objs,u2cw/updater/app.elf,$(U2CW_UPDATER_SOURCES)): obj_cflags += $(TELINK_CFLAGS)
$(call lib-objs,u2cw/updater/app.elf,$(TELINK_SDK_SOURCES)): obj_cppflags += $(TELINK_INCLUDES)
$(call lib-objs,u2cw/updater/app.elf,$(TELINK_SDK_C_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS) -w
$(call lib-objs,u2cw/updater/app.elf,$(TELINK_SDK_S_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS)
$(call lib-objs,u2cw/updater/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cppflags += $(BTSTACK_RENAMES) $(TELINK_BTSTACK_INCLUDES)
$(call lib-objs,u2cw/updater/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cflags += $(TELINK_CFLAGS)

TARGETS += u2cw/updater/app.bin
u2cw/updater/app.bin_type := telink-bin
u2cw/updater/app.bin_deps := $(O)/u2cw/updater/app.elf

TARGETS += u2cw/updater/firmware.bin
u2cw/updater/firmware.bin_type := telink-fw
u2cw/updater/firmware.bin_deps := $(O)/u2cw/updater/app.bin $(JOLLYMAAM_CLI)
$(O)/u2cw/updater/firmware.bin: private telink_stamp := 6a0000005d02
$(O)/u2cw/updater/firmware.bin: private telink_app_start := 0x6000
$(O)/u2cw/updater/firmware.bin: private telink_app_end := 0x16000

# --- Pad: header at 0x16000, app at 0x16120 up to the settings at 0x6f000

U2CW_PAD_FLAGS := -DBOARD_PID=\"U2C_WIRELESS\" -DAPP_START=0x16120 $(U2CW_DEFINES) $(EXTRA_DEFINES) $(BUILD_ID_CPPFLAGS) -include $(TELINK_APP_CONFIG)

U2CW_PAD_SOURCES :=
U2CW_PAD_SOURCES += src/boards/8bitdo-ultimate2c-wireless/board_pad.c
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
U2CW_PAD_SOURCES += src/core/leds_mono.c
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
U2CW_PAD_SOURCES += src/core/quick.c
U2CW_PAD_SOURCES += src/core/macro.c
U2CW_PAD_SOURCES += src/core/settings.c
U2CW_PAD_SOURCES += src/core/settings_ux.c
U2CW_PAD_SOURCES += src/core/usb.c
U2CW_PAD_SOURCES += src/core/utils.c
U2CW_PAD_SOURCES += src/core/battery.c

TARGETS += u2cw/pad/boot.link
u2cw/pad/boot.link_type := telink-boot-link
u2cw/pad/boot.link_deps := $(TELINK_SDK)/project/tlsr_tc32/B80/boot.link
$(O)/u2cw/pad/boot.link: private telink_app_start := 0x16120

TARGETS += u2cw/pad/app.elf
u2cw/pad/app.elf_toolchain := TELINK_B80_
u2cw/pad/app.elf_sources := $(U2CW_PAD_SOURCES) $(TELINK_SDK_SOURCES) $(TELINK_BTSTACK_SOURCES)
u2cw/pad/app.elf_cppflags := $(U2CW_PAD_FLAGS)
u2cw/pad/app.elf_ldflags := $(U2CW_LDFLAGS) -T $(O)/u2cw/pad/boot.link -Map $(O)/u2cw/pad/app.map
u2cw/pad/app.elf_deps := $(O)/u2cw/pad/boot.link $(TELINK_ARCHIVES)
u2cw/pad/app.elf_order_deps := $(BUILD_ID_H)
$(call lib-objs,u2cw/pad/app.elf,$(U2CW_PAD_SOURCES)): obj_cppflags += $(U2CW_INCLUDES)
$(call lib-objs,u2cw/pad/app.elf,$(U2CW_PAD_SOURCES)): obj_cflags += $(TELINK_CFLAGS)
$(call lib-objs,u2cw/pad/app.elf,$(TELINK_SDK_SOURCES)): obj_cppflags += $(TELINK_INCLUDES)
$(call lib-objs,u2cw/pad/app.elf,$(TELINK_SDK_C_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS) -w
$(call lib-objs,u2cw/pad/app.elf,$(TELINK_SDK_S_SOURCES)): obj_cflags += $(TELINK_SDK_CFLAGS)
$(call lib-objs,u2cw/pad/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cppflags += $(BTSTACK_RENAMES) $(TELINK_BTSTACK_INCLUDES)
$(call lib-objs,u2cw/pad/app.elf,$(TELINK_BTSTACK_SOURCES)): obj_cflags += $(TELINK_CFLAGS)

TARGETS += u2cw/pad/app.bin
u2cw/pad/app.bin_type := telink-bin
u2cw/pad/app.bin_deps := $(O)/u2cw/pad/app.elf

TARGETS += u2cw/pad/firmware.bin
u2cw/pad/firmware.bin_type := telink-fw
u2cw/pad/firmware.bin_deps := $(O)/u2cw/pad/app.bin $(JOLLYMAAM_CLI)
$(O)/u2cw/pad/firmware.bin: private telink_stamp := 6a0000005d02
$(O)/u2cw/pad/firmware.bin: private telink_app_start := 0x16120
$(O)/u2cw/pad/firmware.bin: private telink_app_end := 0x6f000

TARGETS += u2cw/pad/firmware.img
u2cw/pad/firmware.img_type := jollymaam-join
u2cw/pad/firmware.img_deps := $(O)/u2cw/updater/firmware.bin $(O)/u2cw/pad/firmware.bin $(JOLLYMAAM_CLI)
$(O)/u2cw/pad/firmware.img: private jollymaam_join := --pid U2C_WIRELESS --at 0x10000 --entry 0x16120 \
	--main-out $(O)/u2cw/pad/update.bin $(O)/u2cw/updater/firmware.bin $(O)/u2cw/pad/firmware.bin

# --- Tests

TARGETS += host/test-u2cw/pad
host/test-u2cw/pad_type := host-tests
host/test-u2cw/pad_toolchain := HOST_
host/test-u2cw/pad_sources := $(HOST_TESTS)
host/test-u2cw/pad_cppflags := -DBOARD_PID=\"U2C_WIRELESS\" $(U2CW_DEFINES) $(EXTRA_DEFINES) -DUNITTEST -DSETTINGS_STORE \
	$(SRC_INCLUDES) -iquote src/boards/8bitdo-ultimate2c-wireless
host/test-u2cw/pad_compile.c = $(host-test-compile)
$(call lib-objs,host/test-u2cw/pad,$(HOST_TESTS_LIBJOLLYMAAM)): host_test_libs := $(O)/host/libjollymaam.a
$(call lib-objs,host/test-u2cw/pad,$(HOST_TESTS_LIBJOLLYMAAM)): $(O)/host/libjollymaam.a
$(call lib-objs,host/test-u2cw/pad,$(HOST_TESTS_BTCTRLER)): host_test_ldflags := $(HOST_TESTS_BTCTRLER_LDFLAGS)

TARGETS += host/test-telink-att
host/test-telink-att_toolchain := HOST_
host/test-telink-att_sources := src/platform/telink/att_telink.c $(TELINK_BTSTACK_SOURCES)
host/test-telink-att_cppflags := -DUNITTEST -I src/core -I src/platform/telink $(BTSTACK_INCLUDES)
TRACKED += $(O)/host/telink-att-test.ok
$(O)/host/telink-att-test.ok: $(O)/host/test-telink-att
	$(call lib-run,$< && touch $@,TEST)

# tools/stackdepth.py's pointer calls: the profiles' ops (struct profile words
# 3 begin, 4 end, 5 report, 6 write, 7 host, 8 read), ATT, SDK event, L2CAP and
# host callbacks; the remap read/write paths only for remap targets; the store
# finds its log once, at boot.
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
# Through ll_push_tx_fifo_handler (a stub that looks like a call to itself).
U2CW_STACK_RULES += blc_l2cap_pushData_2_controller=bls_ll_pushTxFifo
U2CW_STACK_RULES += $(foreach f,set_bond_store set_pairing_store set_cal_store renew_identity set_init write_target,\
	$(f)>>release $(f)>>load $(f)>>stored_field.clone.2)
U2CW_STACK_RULES += find_log<<set_init

u2cw/pad-stackcheck: $(O)/u2cw/pad/app.elf
	$(Q)python3 -I tools/stackdepth.py --tools $(TELINK_TOOLS) $< $(foreach r,$(U2CW_STACK_RULES),'$(r)')
