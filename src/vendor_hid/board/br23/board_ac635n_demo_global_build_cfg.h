#ifndef CONFIG_BOARD_AC635N_DEMO_POST_BUILD_CFG_H
#define CONFIG_BOARD_AC635N_DEMO_POST_BUILD_CFG_H

/* TRANSLATED: Only put settings related to generating isd_config.ini in this file. */
/* TRANSLATED: Do not add unrelated configuration here. */

#ifdef CONFIG_BOARD_AC635N_DEMO

/* Following Macros Affect Periods Of Both Code Compiling And Post-build */

#define CONFIG_DOUBLE_BANK_ENABLE               0       // TRANSLATED: Select single/dual-bank updates. Enabling this changes the flash layout to dual-bank for third-party OTA (also supported by JL-OTA); set CONFIG_FLASH_SIZE to the actual flash capacity.
#define CONFIG_APP_OTA_ENABLE                   0       // TRANSLATED: Support RCSP updates (JL-OTA).

#define CONFIG_UPDATE_JUMP_TO_MASK              0   	// TRANSLATED: Enter the update loader by 0=reset or 1=jump. Jump supports GPIO-held power; first shut down all DMA peripherals.

#define CONFIG_LP_TOUCH_KEY_EN					0		// TRANSLATED: Use the built-in touch controller.
#define CONFIG_UPDATE_WITH_MD5_CHECK_EN			0 		// TRANSLATED: Support MD5 verification during updates.

#define CONFIG_ANC_ENABLE           			0		// TRANSLATED: Support ANC.

//flash size vaule definition
#define FLASH_SIZE_256K							0x40000
#define FLASH_SIZE_512K							0x80000
#define FLASH_SIZE_1M							0x100000
#define FLASH_SIZE_2M							0x200000
#define FLASH_SIZE_4M							0x400000

#define CONFIG_FLASH_SIZE                       FLASH_SIZE_1M    // TRANSLATED: Flash capacity.

/* Above Macros Affect Periods Of Both Code Compiling And Post-build */

/* Following Macros Only For Post Bulid Configuaration */

#define CONFIG_DB_UPDATE_DATA_GENERATE_EN       0       // TRANSLATED: Generate db_data.bin for third-party protocol integration.
#define CONFIG_ONLY_GRENERATE_ALIGN_4K_CODE     1    	// TRANSLATED: Generate only one 4-KiB-aligned code copy in the UFW.

//config for supported chip version
#define CONFIG_SUPPORTED_CHIP_VERSION			A,B,D

//DON'T MODIFY THIS CONFIG EXCEPT SDK PUBLISHER
#define CONFIG_CHIP_NAME                        AC635N              // TRANSLATED: Only the SDK publisher should change this.
//it can be modified before first programming,but keep the same as the original version
#define CONFIG_PID                              AC635N              // TRANSLATED: May be changed before initial flashing or forced update; subsequent updates must keep it consistent.
//it can be modified before first programming,but keep the same as the original version
#define CONFIG_VID                              0.01				// TRANSLATED: May be changed before initial flashing or forced update; subsequent updates must keep it consistent.

//Project with bluetooth,it must use OSC as PLL_SOURCE;
#define CONFIG_PLL_SOURCE_USING_LRC             0       			// TRANSLATED: PLL clock source: 1=LRC, 2=OSC.

//config alignment size unit
#ifdef CONFIG_256K_FLASH
#define ALIGN_UNIT_256B             1                 				// TRANSLATED: Flash alignment; choose 256-byte alignment for 256-KiB flash.
#else
#define ALIGN_UNIT_256B             0
#endif

//partial platform check this config to select the uart IO for wired update
#define CONFIG_UART_UPDATE_PIN                  PB05

//isd_download loader/uboot/update_loader debug io config
//#define CONFIG_UBOOT_DEBUG_PIN                  PA05
//#define CONFIG_UBOOT_DEBUG_BAUD_RATE            1000000

//config long-press reset io pin,time,trigger level
#if CONFIG_LP_TOUCH_KEY_EN
#define CONFIG_RESET_PIN                        LDO  //io pin
#define CONFIG_RESET_TIME                       04   //unit:second
#define CONFIG_RESET_LEVEL                      1	 //tigger level(0/1)
#else
#define CONFIG_RESET_PIN                        PB01 //io pin
#define CONFIG_RESET_TIME                       08   //unit:second
#define CONFIG_RESET_LEVEL                      0	 //tigger level(0/1)
#endif

//reserved three custom cfg item for the future definition
//#define CONFIG_CUSTOM_CFG1_TYPE			POWER_PIN
//#define CONFIG_CUSTOM_CFG1_VALUE		 	PC01_1

//#define CONFIG_CUSTOM_CFG2_TYPE
//#define CONFIG_CUSTOM_CFG2_VALUE

//#define CONFIG_CUSTOM_CFG3_TYPE
//#define CONFIG_CUSTOM_CFG3_VALUE

// TRANSLATED: Optional CONFIG_VDDIO_LVD_LEVEL=4; levels 0..7 correspond to 1.9..2.6 V in 0.1 V steps.

//with single-bank mode,actual vm size should larger this VM_LEAST_SIZE,and dual bank mode,actual vm size equals this;
#define CONFIG_VM_LEAST_SIZE                    8K
//config whether erased this area when do a update,1-No Operation,0-Erase
#define CONFIG_VM_OPT							1

//config whether erased this area when do a update,1-No Operation,0-Erase
#define CONFIG_BTIF_OPT							1

//reserved two custom cfg area for the future definition
//#define CONFIG_RESERVED_AREA1					EXIF1
#ifdef CONFIG_RESERVED_AREA1
#define CONFIG_RESERVED_AREA1_ADDR				AUTO
#define CONFIG_RESERVED_AREA1_LEN				0x1000
#define CONFIG_RESERVED_AREA1_OPT				1
//#define CONFIG_RESERVED_AREA1_FILE				anc_gains.bin
#endif

//#define CONFIG_RESERVED_AREA2					EXIF2
#ifdef CONFIG_RESERVED_AREA2
#define CONFIG_RESERVED_AREA2_ADDR				AUTO
#define CONFIG_RESERVED_AREA2_LEN				0x1000
#define CONFIG_RESERVED_AREA2_OPT				1
//#define CONFIG_RESERVED_AREA2_FILE				anc_gains.bin
#endif

/* Above Macros Only For Post Bulid Configuaration */
#endif

#endif
