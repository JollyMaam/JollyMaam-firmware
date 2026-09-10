# BR23 compiler ABI and library configuration extracted from the pinned Jieli
# SDK. This project owns its targets and never includes a vendor app Makefile.

CC := $(TOOL_DIR)/clang
LD := $(TOOL_DIR)/lto-wrapper
OBJDUMP := $(TOOL_DIR)/objdump
OBJCOPY := $(TOOL_DIR)/objcopy
NM := $(TOOL_DIR)/nm
SYS_LIB_DIR := $(TOOL_DIR)/../lib/r3
SYS_INC_DIR := $(TOOL_DIR)/../include

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
	-w

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
	-DVM_MAX_SIZE_CONFIG=16*1024

SDK_INCLUDES := \
	-I$(PROJECT_ROOT)/src \
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
	-I$(SYS_INC_DIR)

SDK_ARCHIVES := \
	$(SDK)/cpu/br23/liba/cpu.a \
	$(SDK)/cpu/br23/liba/system.a

SDK_RUNTIME_LIBS := \
	$(SYS_LIB_DIR)/libm.a \
	$(SYS_LIB_DIR)/libc.a \
	$(SYS_LIB_DIR)/libcompiler-rt.a

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
	--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar,setup_arch \
	--plugin-opt=save-temps \
	--plugin-opt=-pi32v2-enable-rep-memop \
	--plugin-opt=-warn-stack-size=256 \
	--sort-common \
	--dont-complain-call-overflow \
	--gc-sections \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-mattr=+fprev1
