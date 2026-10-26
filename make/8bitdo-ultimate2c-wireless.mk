# 8BitDo Ultimate 2C Wireless: the pad, a Telink TLSR8208 (B80).
#   8bitdo-ultimate2c-wireless-pad          firmware.bin in $(O)/<target>/
#   8bitdo-ultimate2c-wireless-pad-flash    through the 8BitDo boot loader
#   8bitdo-ultimate2c-wireless-pad-loader   into the boot loader
#   8bitdo-ultimate2c-wireless-pad-test, 8bitdo-ultimate2c-wireless-all
# The boot loader is 0x0000-0x5fff and starts the app at 0x6000; stock
# keeps data in sectors from 0x73000 (docs/8bitdo-ultimate2c-wireless/),
# which nothing here writes.

U2CW_PAD := 8bitdo-ultimate2c-wireless-pad
$(U2CW_PAD)_FLASH_SIZE := 0x80000
$(U2CW_PAD)_APP_START := 0x6000
$(U2CW_PAD)_APP_END := 0x73000
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
$(U2CW_PAD)_DEFINES += -DPROBE_EDGES=0  # controller_lite has no latency probe
$(U2CW_PAD)_DEFINES += -DREMAPS_STORED=4
$(U2CW_PAD)_DEFINES += -DREMAP_RULES=8
$(U2CW_PAD)_DEFINES += -DRECORD_MAX=152
$(U2CW_PAD)_DEFINES += -DUX_REMAP_CONFIG=0
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
$(U2CW_PAD)_SOURCES += src/core/input.c
$(U2CW_PAD)_SOURCES += src/core/input_calibration.c
$(U2CW_PAD)_SOURCES += src/core/remap.c
$(U2CW_PAD)_SOURCES += src/core/settings.c
$(U2CW_PAD)_SOURCES += src/core/settings_ux.c
$(U2CW_PAD)_SOURCES += src/core/usb.c
$(U2CW_PAD)_SOURCES += src/core/utils.c

$(eval $(call telink_board,$(U2CW_PAD)))$(eval $(call host_tests,$(U2CW_PAD)))

.PHONY: 8bitdo-ultimate2c-wireless-all 8bitdo-ultimate2c-wireless-test
8bitdo-ultimate2c-wireless-all: $(U2CW_PAD)
8bitdo-ultimate2c-wireless-test: $(U2CW_PAD)-test

# Stock receiver entry only: no receiver firmware target or flashing path yet.
.PHONY: 8bitdo-ultimate2c-wireless-dongle-loader 8bitdoboot-test
8bitdo-ultimate2c-wireless-dongle-loader:
	$(Q)python3 -I $(TELINK_IMAGE_TOOL) loader --pid U2C_RECEIVER $(if $(DEVICE),--device $(DEVICE))

8bitdoboot-test:
	$(Q)python3 -I tools/test_8bitdoboot.py

$(U2CW_PAD)-test: | 8bitdoboot-test

.PHONY: telink-att-test
$(U2CW_PAD)-test: $(O)/host/test-telink-att
telink-att-test: $(O)/host/test-telink-att
	$(Q)$<

$(O)/host/test-telink-att: src/platform/telink/att_telink.c src/platform/telink/att_telink.h \
    src/platform/telink/btstack_config.h src/core/pad.h $(BTSTACK)/src/ble/att_db.c $(BTSTACK)/src/btstack_util.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DUNITTEST -I src/core -I src/platform/telink $(BTSTACK_INCLUDES) \
	  src/platform/telink/att_telink.c $(BTSTACK)/src/ble/att_db.c $(BTSTACK)/src/btstack_util.c -o $@
