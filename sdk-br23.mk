# BR23 compiler ABI and library configuration extracted from the pinned Jieli
# SDK. This project owns its targets and never includes a vendor app Makefile.
#
# Mostly copied from apps/hid/board/br23/Makefile

CC := $(TOOL_DIR)/clang
LD := $(TOOL_DIR)/lto-wrapper
OBJDUMP := $(TOOL_DIR)/objdump
OBJCOPY := $(TOOL_DIR)/objcopy
NM := $(TOOL_DIR)/nm
SYS_LIB_DIR := $(TOOL_DIR)/../lib/r3
SYS_INC_DIR := $(TOOL_DIR)/../include

SDK_UBOOT := $(SDK)/cpu/br23/tools/uboot.boot
SDK_CONFIG_TOOL := $(SDK)/cpu/br23/tools/cfg_tool.bin

SDK_CFLAGS := \
	-target pi32v2 \
	-mcpu=r3 \
	-integrated-as \
	-flto \
	-Oz \
	-g \
	-Wuninitialized \
	-Wno-invalid-noreturn \
	-fno-common \
	-fallow-pointer-null \
	-fprefer-gnu-section \
	-Wno-shift-negative-value \
	-Wundef \
	-Wframe-larger-than=256 \
	-Wincompatible-pointer-types \
	-Wreturn-type \
	-Wimplicit-function-declaration \
	-fms-extensions \
	-w \


SDK_DEFINES := \
	-DAPP_ASYNC_POOL_NUM_CONFIG=0 \
	-DCONFIG_AEC_M=640 \
	-DCONFIG_AUDIO_ONCHIP \
	-DCONFIG_BTCTRLER_TASK_DEL_ENABLE \
	-DCONFIG_CPU_BR23 \
	-DCONFIG_DNS_ENABLE \
	-DCONFIG_EQ_SUPPORT_ASYNC \
	-DCONFIG_EVENT_KEY_MAP_ENABLE=0 \
	-DCONFIG_FREE_RTOS_ENABLE \
	-DCONFIG_HID_CASE_ENABLE \
	-DCONFIG_ITEM_FORMAT_VM \
	-DCONFIG_LINK_DISTURB_SCAN_ENABLE=0 \
	-DCONFIG_LITE_AEC_ENABLE=0 \
	-DCONFIG_LITE_AUDIO \
	-DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE \
	-DCONFIG_LMP_RESET_ENABLE \
	-DCONFIG_MEDIA_DEVELOP_ENABLE \
	-DCONFIG_MIXER_CYCLIC \
	-DCONFIG_MMU_ENABLE \
	-DCONFIG_MSBC_CODEC_HW \
	-DCONFIG_NEW_ECC_ENABLE \
	-DCONFIG_OTA_UPDATA_ENABLE \
	-DCONFIG_RELEASE_ENABLE \
	-DCONFIG_SBC_CODEC_HW \
	-DCONFIG_TRANSFER_ENABLE \
	-DCONFIG_TWS_ENABLE \
	-DCONFIG_UPDATA_ENABLE \
	-DEVENT_HANDLER_NUM_CONFIG=2 \
	-DEVENT_POOL_SIZE_CONFIG=256 \
	-DEVENT_TOUCH_ENABLE_CONFIG=0 \
	-D__GCC_PI32V2__ \
	-DSDFILE_VERSION=0x020000 \
	-DSDK_VERSION_CFG_DEFINE=0x240001 \
	-DSDK_VERSION_DATE_DEFINE=20241231 \
	-D__SHELL__ \
	-DSUPPORT_MS_EXTENSIONS \
	-DTCFG_APP_BT_EN=1 \
	-DTIMER_POOL_NUM_CONFIG=10 \
	-DUSB_PC_NO_APP_MODE \
	-DUSE_SDFILE_NEW=1 \
	-DVFS_ENABLE=1 \
	-DVFS_FILE_POOL_NUM_CONFIG=0 \
	-DVM_ITEM_MAX_NUM=256 \
	-DVM_MAX_SIZE_CONFIG=16*1024 \


SDK_INCLUDES := \
	-I$(SDK)/include_lib \
	-I$(SDK)/include_lib/driver \
	-I$(SDK)/include_lib/driver/device \
	-I$(SDK)/include_lib/driver/cpu/br23 \
	-I$(SDK)/include_lib/system \
	-I$(SDK)/include_lib/system/generic \
	-I$(SDK)/include_lib/system/device \
	-I$(SDK)/include_lib/system/fs \
	-I$(SDK)/include_lib/system/ui \
	-I$(SDK)/include_lib/system/math/cpu/br23 \
	-I$(SDK)/include_lib/btctrler \
	-I$(SDK)/include_lib/btctrler/port/br23 \
	-I$(SDK)/include_lib/update \
	-I$(SDK)/include_lib/agreement \
	-I$(SDK)/include_lib/btstack \
	-I$(SDK)/include_lib/btstack/third_party/common \
	-I$(SDK)/include_lib/btstack/third_party/rcsp \
	-I$(SDK)/include_lib/media/media_develop \
	-I$(SDK)/include_lib/media/media_develop/media \
	-I$(SDK)/include_lib/media/media_develop/media/cpu/br23 \
	-I$(SDK)/include_lib/media/media_develop/media/cpu/br23/asm \
	-I$(SDK)/include_lib/ai_stack/JL_rcsp \
	-I$(SDK)/include_lib/system/ex_mcu \
	-I$(SDK)/apps/common \
	-I$(SDK)/apps/common/include \
	-I$(SDK)/apps/common/third_party_profile/jieli \
	-I$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp \
	-I$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata \
	-I$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp/bt_trans_data \
	-I$(SDK)/apps/common/third_party_profile/jieli/gatt_common \
	-I$(SDK)/apps/common/third_party_profile/jieli/online_db \
	-I$(SDK)/apps/common/third_party_profile/jieli/trans_data_demo \
	-I$(SDK)/apps/common/third_party_profile/common \
	-I$(SDK)/apps/common/device \
	-I$(SDK)/apps/common/device/usb \
	-I$(SDK)/apps/common/device/usb/device \
	-I$(SDK)/apps/common/device/usb/host \
	-I$(SDK)/apps/common/audio \
	-I$(SDK)/apps/common/code_switch/include \
	-I$(SDK)/apps/common/device/optical_mouse_sensor/include \
	-I$(SDK)/apps/common/device/touch_pad \
	-I$(SDK)/cpu/br23 \
	-I$(SDK)/cpu/br23/audio_dec \
	-I$(SDK)/cpu/br23/audio_enc \
	-I$(SYS_INC_DIR) \


SDK_RUNTIME_LIBS := \
	$(SYS_LIB_DIR)/libm.a \
	$(SYS_LIB_DIR)/libc.a \
	$(SYS_LIB_DIR)/libcompiler-rt.a \


SDK_LFLAGS := \
	--plugin-opt=-pi32v2-always-use-itblock=false \
	--plugin-opt=-enable-ipra=true \
	--plugin-opt=-pi32v2-merge-max-offset=4096 \
	--plugin-opt=-pi32v2-enable-simd=true \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-global-merge-on-const \
	--plugin-opt=-inline-threshold=5 \
	--plugin-opt=-inline-max-allocated-size=32 \
	--plugin-opt=-inline-normal-into-special-section=true \
	--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar \
	--plugin-opt=save-temps \
	--plugin-opt=-pi32v2-enable-rep-memop \
	--plugin-opt=-warn-stack-size=256 \
	--sort-common \
	--dont-complain-call-overflow \
	--gc-sections \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-mattr=+fprev1 \


# apps/common portion of the SDK's unmodified BR23 HID build.  The application
# and board-specific sources themselves live in src/vendor_hid.
SDK_COMMON_SOURCES_FULL := \
	$(SDK)/apps/common/audio/audio_utils.c \
	$(SDK)/apps/common/audio/decode/decode.c \
	$(SDK)/apps/common/audio/sine_make.c \
	$(SDK)/apps/common/bt_common/bt_test_api.c \
	$(SDK)/apps/common/code_switch/code_switch.c \
	$(SDK)/apps/common/debug/debug.c \
	$(SDK)/apps/common/device/key/adkey.c \
	$(SDK)/apps/common/device/key/ctmu_touch_key.c \
	$(SDK)/apps/common/device/key/iokey.c \
	$(SDK)/apps/common/device/key/irkey.c \
	$(SDK)/apps/common/device/key/key_driver.c \
	$(SDK)/apps/common/device/key/matrix_keyboard.c \
	$(SDK)/apps/common/device/key/matrix_keyboard_ex_mcu.c \
	$(SDK)/apps/common/device/key/touch_key.c \
	$(SDK)/apps/common/device/optical_mouse_sensor/OMSensor_manage.c \
	$(SDK)/apps/common/device/optical_mouse_sensor/hal3205/hal3205.c \
	$(SDK)/apps/common/device/optical_mouse_sensor/hal3212/hal3212.c \
	$(SDK)/apps/common/device/touch_pad/SYD9557M.c \
	$(SDK)/apps/common/device/usb/device/cdc.c \
	$(SDK)/apps/common/device/usb/device/custom_hid.c \
	$(SDK)/apps/common/device/usb/device/descriptor.c \
	$(SDK)/apps/common/device/usb/device/hid.c \
	$(SDK)/apps/common/device/usb/device/msd.c \
	$(SDK)/apps/common/device/usb/device/msd_upgrade.c \
	$(SDK)/apps/common/device/usb/device/task_pc.c \
	$(SDK)/apps/common/device/usb/device/uac1.c \
	$(SDK)/apps/common/device/usb/device/uac_stream.c \
	$(SDK)/apps/common/device/usb/device/usb_device.c \
	$(SDK)/apps/common/device/usb/device/user_setup.c \
	$(SDK)/apps/common/device/usb/host/adb.c \
	$(SDK)/apps/common/device/usb/host/aoa.c \
	$(SDK)/apps/common/device/usb/host/audio.c \
	$(SDK)/apps/common/device/usb/host/audio_demo.c \
	$(SDK)/apps/common/device/usb/host/hid.c \
	$(SDK)/apps/common/device/usb/host/usb_bulk_transfer.c \
	$(SDK)/apps/common/device/usb/host/usb_ctrl_transfer.c \
	$(SDK)/apps/common/device/usb/host/usb_host.c \
	$(SDK)/apps/common/device/usb/host/usb_storage.c \
	$(SDK)/apps/common/device/usb/usb_config.c \
	$(SDK)/apps/common/device/usb/usb_host_config.c \
	$(SDK)/apps/common/ex_mcu/ex_mcu_uart.c \
	$(SDK)/apps/common/jl_kws/jl_kws_algo.c \
	$(SDK)/apps/common/jl_kws/jl_kws_audio.c \
	$(SDK)/apps/common/jl_kws/jl_kws_event.c \
	$(SDK)/apps/common/jl_kws/jl_kws_main.c \
	$(SDK)/apps/common/temp_trim/dtemp_pll_trim.c \
	$(SDK)/apps/common/third_party_profile/common/custom_cfg.c \
	$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp/bt_trans_data/rcsp_hid_inter.c \
	$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_bluetooth.c \
	$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata/rcsp_ch_loader_download.c \
	$(SDK)/apps/common/third_party_profile/jieli/JL_rcsp/rcsp_updata/rcsp_user_update.c \
	$(SDK)/apps/common/third_party_profile/jieli/gatt_common/le_gatt_client.c \
	$(SDK)/apps/common/third_party_profile/jieli/gatt_common/le_gatt_common.c \
	$(SDK)/apps/common/third_party_profile/jieli/gatt_common/le_gatt_server.c \
	$(SDK)/apps/common/third_party_profile/jieli/online_db/online_db_deal.c \
	$(SDK)/apps/common/third_party_profile/jieli/online_db/spp_online_db.c \
	$(SDK)/apps/common/update/testbox_update.c \
	$(SDK)/apps/common/update/uart_update.c \
	$(SDK)/apps/common/update/uart_update_master.c \
	$(SDK)/apps/common/update/update.c \


SDK_SOURCES_FULL := \
	$(SDK)/cpu/br23/adc_api.c \
	$(SDK)/cpu/br23/audio_common/app_audio.c \
	$(SDK)/cpu/br23/audio_dec/audio_dec.c \
	$(SDK)/cpu/br23/audio_dec/audio_dec_midi_ctrl.c \
	$(SDK)/cpu/br23/audio_dec/audio_dec_pc.c \
	$(SDK)/cpu/br23/audio_dec/audio_dec_tone.c \
	$(SDK)/cpu/br23/audio_dec/audio_usb_mic.c \
	$(SDK)/cpu/br23/audio_dec/tone_player.c \
	$(SDK)/cpu/br23/audio_demo/audio_dac_io_demo.c \
	$(SDK)/cpu/br23/audio_demo/audio_fft_demo.c \
	$(SDK)/cpu/br23/audio_enc/audio_adc_demo.c \
	$(SDK)/cpu/br23/audio_enc/audio_enc.c \
	$(SDK)/cpu/br23/audio_enc/audio_mic_codec.c \
	$(SDK)/cpu/br23/charge.c \
	$(SDK)/cpu/br23/chargestore.c \
	$(SDK)/cpu/br23/clock_manager.c \
	$(SDK)/cpu/br23/ctmu.c \
	$(SDK)/cpu/br23/handshake_timer.c \
	$(SDK)/cpu/br23/iic_eeprom_test.c \
	$(SDK)/cpu/br23/iic_hw.c \
	$(SDK)/cpu/br23/iic_slave_test.c \
	$(SDK)/cpu/br23/iic_soft.c \
	$(SDK)/cpu/br23/irflt.c \
	$(SDK)/cpu/br23/led_spi.c \
	$(SDK)/cpu/br23/ledc_test.c \
	$(SDK)/cpu/br23/mcpwm.c \
	$(SDK)/cpu/br23/pap.c \
	$(SDK)/cpu/br23/plcnt.c \
	$(SDK)/cpu/br23/port_wkup.c \
	$(SDK)/cpu/br23/pwm_led.c \
	$(SDK)/cpu/br23/setup.c \
	$(SDK)/cpu/br23/spi.c \
	$(SDK)/cpu/br23/spi_test.c \
	$(SDK)/cpu/br23/uart_dev.c \
	$(SDK)/cpu/br23/uart_test.c \


SDK_ARCHIVES_FULL := \
	$(SDK)/cpu/br23/liba/cpu.a \
	$(SDK)/cpu/br23/liba/system.a \
	$(SDK)/cpu/br23/liba/btstack.a \
	$(SDK)/cpu/br23/liba/btctrler.a \
	$(SDK)/cpu/br23/liba/crypto_toolbox_Osize.a \
	$(SDK)/cpu/br23/liba/rcsp_stack.a \
	$(SDK)/cpu/br23/liba/agreement.a \
	$(SDK)/cpu/br23/liba/aec.a \
	$(SDK)/cpu/br23/liba/media.a \
	$(SDK)/cpu/br23/liba/media_app.a \
	$(SDK)/cpu/br23/liba/libAptFilt_pi32v2_OnChip.a \
	$(SDK)/cpu/br23/liba/libEchoSuppress_pi32v2_OnChip.a \
	$(SDK)/cpu/br23/liba/libNoiseSuppress_pi32v2_OnChip.a \
	$(SDK)/cpu/br23/liba/libSplittingFilter_pi32v2_OnChip.a \
	$(SDK)/cpu/br23/liba/libFFT_pi32v2_OnChip.a \
	$(SDK)/cpu/br23/liba/libOpcore_maskrom_pi32v2_OnChip.a \
	$(SDK)/cpu/br23/liba/lib_resample_cal.a \
	$(SDK)/cpu/br23/liba/lib_resample_fast_cal.a \
	$(SDK)/cpu/br23/liba/opus_enc_lib.a \
	$(SDK)/cpu/br23/liba/speex_enc_lib.a \
	$(SDK)/cpu/br23/liba/lib_adpcm_ima_code.a \
	$(SDK)/cpu/br23/liba/lib_adpcm_code.a \
	$(SDK)/cpu/br23/liba/lc3_codec_lib.a \
	$(SDK)/cpu/br23/liba/wav_dec_lib.a \
	$(SDK)/cpu/br23/liba/bfilterfun_lib.a \
	$(SDK)/cpu/br23/liba/wtg_dec_lib.a \
	$(SDK)/cpu/br23/liba/wtgv2_dec_lib.a \
	$(SDK)/cpu/br23/liba/wav_dec_lib.a  \
	$(SDK)/cpu/br23/liba/mp3_dec_lib.a \
	$(SDK)/cpu/br23/liba/lib_pitch_speed.a \
	$(SDK)/cpu/br23/liba/lib_dns.a \
	$(SDK)/cpu/br23/liba/lib_midi_dec.a \
	$(SDK)/cpu/br23/liba/update.a \
