# 8BitDo Ultimate 2C Wireless: the pad, a Telink TLSR8208 (B80).
#   8bitdo-ultimate2c-wireless-flash        the pad: its updater and the app
#                                           above it, through the 8BitDo boot loader
#   8bitdo-ultimate2c-wireless-pad          update.bin in $(O)/<target>/: the
#                                           app's header and the app, above its updater
#   8bitdo-ultimate2c-wireless-pad-updater  the pad's updater: flashed with -pad
#   8bitdo-ultimate2c-wireless-pad-loader   into the boot loader
#   8bitdo-ultimate2c-wireless-pad-stack    the deepest stack use against the room
#                                           for it (part of -pad-test)
#   8bitdo-ultimate2c-wireless-pad-test, 8bitdo-ultimate2c-wireless-all
# The boot loader is 0x0000-0x5fff and starts the updater at 0x6000; stock
# keeps data in sectors from 0x73000 (docs/8bitdo-ultimate2c-wireless/),
# which nothing here writes.  The updater has 64 KiB, the app's header is at
# 0x16000 and the app after it; settings and bonds stay at 0x6f000.

U2CW_PAD := 8bitdo-ultimate2c-wireless-pad
$(U2CW_PAD)_FLASH_SIZE := 0x80000
$(U2CW_PAD)_APP_START := 0x16120
$(U2CW_PAD)_APP_END := 0x6f000
$(U2CW_PAD)_PID := U2C_WIRELESS
$(U2CW_PAD)_STAMP := 6a0000005d02
$(U2CW_PAD)_LOADER_HINT := If USB boot entry fails: USB unplugged, hold L1+R1, tap Home, plug in USB.
$(U2CW_PAD)_BOARD_DIR := src/boards/8bitdo-ultimate2c-wireless
$(U2CW_PAD)_LDFLAGS := --wrap=otp_read --wrap=bls_smp_param_saveBondingInfo --wrap=l2cap_att_handler
# Root the replacement explicitly: --defsym alone does not retain it with
# this linker's --gc-sections (the alias would otherwise resolve to zero).
$(U2CW_PAD)_LDFLAGS += --wrap=blt_connect --undefined=plat_ll_connect_request
$(U2CW_PAD)_LDFLAGS += --defsym=blc_ll_procConnectReq=plat_ll_connect_request
$(U2CW_PAD)_LDFLAGS += --wrap=blc_ll_get_connEffectiveMaxTxOctets
$(U2CW_PAD)_LDFLAGS += --undefined=plat_ll_push_tx_fifo --defsym=bls_ll_pushTxFifo=plat_ll_push_tx_fifo

# 16 KiB of SRAM: the settings keep slots A-Y and remap slots A-Y with the
# base, 8 rules each; records sized for them and a small staging buffer;
# small log and trace rings.
$(U2CW_PAD)_DEFINES :=
$(U2CW_PAD)_DEFINES += -DSLOTS_STORED=4
# The CPU at 48 MHz: code runs from flash through a 2 KiB cache, and a pass
# of the control loop's input steps took ~1.8 ms at 24 MHz, which the just
# in time reports must wait out (U2CW_CPU_HZ=24000000 for the old clock).
U2CW_CPU_HZ ?= 48000000
$(U2CW_PAD)_DEFINES += -DCLOCK_SYS_CLOCK_HZ=$(U2CW_CPU_HZ)
# The latency probe (padtool latency) is off: its ring fits only in the
# spare RAM in .data, for test builds (make ... U2CW_PROBE_EDGES=6).
U2CW_PROBE_EDGES ?= 0
$(U2CW_PAD)_DEFINES += -DPROBE_EDGES=$(U2CW_PROBE_EDGES)
ifneq ($(U2CW_PROBE_EDGES),0)
$(U2CW_PAD)_DEFINES += -DPROBE_IN_DATA
endif
# The sampling profiler (padtool profile) is off too: its ring takes the
# same spare RAM, for test builds (make ... U2CW_PROFILE=1).
U2CW_PROFILE ?= 0
U2CW_PROFILE_RING ?= 28
ifneq ($(U2CW_PROFILE),0)
ifneq ($(U2CW_PROBE_EDGES),0)
$(error U2CW_PROFILE and U2CW_PROBE_EDGES share the spare RAM: one at a time)
endif
$(U2CW_PAD)_DEFINES += -DPROF_RING=$(U2CW_PROFILE_RING)
endif
$(U2CW_PAD)_DEFINES += -DREMAPS_STORED=4
$(U2CW_PAD)_DEFINES += -DREMAP_RULES=8
$(U2CW_PAD)_DEFINES += -DRECORD_MAX=152
$(U2CW_PAD)_DEFINES += -DSLOT_USB_AUDIO=0
$(U2CW_PAD)_DEFINES += -DSTAGE_MAX=256
$(U2CW_PAD)_DEFINES += -DLOG_LINES=2
$(U2CW_PAD)_DEFINES += -DLOG_TEXT=48
$(U2CW_PAD)_DEFINES += -DTRACE=1
$(U2CW_PAD)_DEFINES += -DUSB_RESULTS=64

$(U2CW_PAD)_SOURCES :=
$(U2CW_PAD)_SOURCES += src/boards/8bitdo-ultimate2c-wireless/board_pad.c
$(U2CW_PAD)_SOURCES += src/platform/telink/platform_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/platform_telink_usb.c
$(U2CW_PAD)_SOURCES += src/platform/telink/libc_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/store_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/bt_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/att_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/connect_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/tx_telink.c
$(U2CW_PAD)_SOURCES += src/platform/telink/rx_telink.c
$(U2CW_PAD)_SOURCES += src/core/controller_lite.c
$(U2CW_PAD)_SOURCES += src/profiles/profile_hid.c
$(U2CW_PAD)_SOURCES += src/profiles/profile_switch.c
$(U2CW_PAD)_SOURCES += src/profiles/profile_xbox.c
$(U2CW_PAD)_SOURCES += src/profiles/profile_playstation.c
$(U2CW_PAD)_SOURCES += src/core/motion.c
$(U2CW_PAD)_SOURCES += src/core/input.c
$(U2CW_PAD)_SOURCES += src/core/input_calibration.c
$(U2CW_PAD)_SOURCES += src/core/remap.c
$(U2CW_PAD)_SOURCES += src/core/settings.c
$(U2CW_PAD)_SOURCES += src/core/settings_ux.c
$(U2CW_PAD)_SOURCES += src/core/usb.c
$(U2CW_PAD)_SOURCES += src/core/utils.c

# The pad's updater (src/updater/, docs/firmware/updater.md): where the boot
# loader starts an app.
U2CW_UPDATER := 8bitdo-ultimate2c-wireless-pad-updater
$(U2CW_UPDATER)_FLASH_SIZE := $($(U2CW_PAD)_FLASH_SIZE)
$(U2CW_UPDATER)_APP_START := 0x6000
$(U2CW_UPDATER)_APP_END := 0x16000
$(U2CW_UPDATER)_UPDATER := 0x10000
$(U2CW_UPDATER)_CODE_LIMIT := $($(U2CW_PAD)_APP_END)
$(U2CW_UPDATER)_PID := $($(U2CW_PAD)_PID)
$(U2CW_UPDATER)_STAMP := $($(U2CW_PAD)_STAMP)
$(U2CW_UPDATER)_LOADER_HINT := $($(U2CW_PAD)_LOADER_HINT)
$(U2CW_UPDATER)_BOARD_DIR := $($(U2CW_PAD)_BOARD_DIR)
$(U2CW_UPDATER)_LDFLAGS := $($(U2CW_PAD)_LDFLAGS)
$(U2CW_UPDATER)_DEFINES := -DLOG_LINES=16 -DLOG_TEXT=48
$(U2CW_UPDATER)_SOURCES :=
$(U2CW_UPDATER)_SOURCES += src/updater/updater.c
$(U2CW_UPDATER)_SOURCES += src/updater/updater_telink.c
$(U2CW_UPDATER)_SOURCES += src/updater/updater_ble_telink.c
$(U2CW_UPDATER)_SOURCES += src/boards/8bitdo-ultimate2c-wireless/board_pad.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/platform_telink.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/platform_telink_usb.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/libc_telink.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/bt_telink.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/att_telink.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/connect_telink.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/tx_telink.c
$(U2CW_UPDATER)_SOURCES += src/platform/telink/rx_telink.c
$(U2CW_UPDATER)_SOURCES += src/core/utils.c

$(U2CW_PAD)_FRONT := $(U2CW_UPDATER)

$(eval $(call telink_board,$(U2CW_PAD)))$(eval $(call host_tests,$(U2CW_PAD)))
$(eval $(call telink_board,$(U2CW_UPDATER)))

.PHONY: 8bitdo-ultimate2c-wireless-all 8bitdo-ultimate2c-wireless-test 8bitdo-ultimate2c-wireless-flash
8bitdo-ultimate2c-wireless-all: $(U2CW_PAD)
8bitdo-ultimate2c-wireless-flash: $(U2CW_PAD)-flash
8bitdo-ultimate2c-wireless-test: $(U2CW_PAD)-test

# Stock receiver entry only: no receiver firmware target or flashing path yet.
.PHONY: 8bitdo-ultimate2c-wireless-dongle-loader 8bitdoboot-test
8bitdo-ultimate2c-wireless-dongle-loader:
	$(Q)python3 -I $(TELINK_IMAGE_TOOL) loader --pid U2C_RECEIVER $(if $(DEVICE),--device $(DEVICE))

8bitdoboot-test:
	$(Q)python3 -I tools/test_8bitdoboot.py

$(U2CW_PAD)-test: | 8bitdoboot-test

# The deepest stack use against the room above .bss, and the interrupts'
# against irq_stk (tools/stackdepth.py).  The pointer calls: the profiles'
# ops (struct profile words 3 begin, 5 report, 6 write, 7 host, 8 read), ATT's
# callbacks, the SDK's event, L2CAP and host callbacks.  The settings paths
# that read in or put out a remap are taken only for remap targets, never
# from the slot, calibration and boot ones; a held remap is the only dirty
# one, so writing it reads nothing in.  The store finds its log once, at boot.
U2CW_OPS = $(foreach p,generic_ble xbox_ble switch2,$(1)=@profile_$(p):$(2))
U2CW_STACK_RULES := $(call U2CW_OPS,bt_events,3) $(call U2CW_OPS,bt_events,6)
U2CW_STACK_RULES += $(call U2CW_OPS,start_session,3) $(call U2CW_OPS,report,5)
U2CW_STACK_RULES += $(call U2CW_OPS,host,7) $(call U2CW_OPS,read_value,8)
U2CW_STACK_RULES += att_handle_request=read_value,write_value
U2CW_STACK_RULES += blt_event_callback_func=on_connect,on_terminate,on_phy,on_length
U2CW_STACK_RULES += $(foreach f,blt_connect bls_procPendingEvent blt_send_adv bls_ll_security_proc,$(f)=blt_event_callback_func)
U2CW_STACK_RULES += blt_ll_packet_proc=blt_event_callback_func,plat_l2cap_receive
U2CW_STACK_RULES += blc_gap_send_event=on_host
U2CW_STACK_RULES += $(foreach f,set_bond_store set_pairing_store set_cal_store renew_identity set_init write_target,\
	$(f)>>release $(f)>>load $(f)>>stored_field.clone.2)
U2CW_STACK_RULES += find_log<<set_init

.PHONY: $(U2CW_PAD)-stack
$(U2CW_PAD)-test: | $(U2CW_PAD)-stack
$(U2CW_PAD)-stack: $(O)/$(U2CW_PAD)/app.elf
	$(Q)python3 -I tools/stackdepth.py --tools $(TELINK_TOOLS) $< $(foreach r,$(U2CW_STACK_RULES),'$(r)')

.PHONY: telink-att-test
$(U2CW_PAD)-test: $(O)/host/test-telink-att
telink-att-test: $(O)/host/test-telink-att
	$(Q)$<

$(O)/host/test-telink-att: src/platform/telink/att_telink.c src/platform/telink/att_telink.h \
    src/platform/telink/btstack_config.h src/core/pad.h $(BTSTACK)/src/ble/att_db.c $(BTSTACK)/src/btstack_util.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DUNITTEST -I src/core -I src/platform/telink $(BTSTACK_INCLUDES) \
	  src/platform/telink/att_telink.c $(BTSTACK)/src/ble/att_db.c $(BTSTACK)/src/btstack_util.c -o $@
