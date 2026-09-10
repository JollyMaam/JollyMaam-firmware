#include "typedef.h"
#include "asm/wdt.h"
#include "system/app_core.h"
#include "system/includes.h"
#include "system/init.h"

#include "board.h"
#include "control.h"
#include "rom.h"

#define CYCLONE2_APP_NAME "cyclone2"
#define ACTION_CYCLONE2_MAIN 0x43593201

/* The SDK implementation is compiled under this private name. Keeping this
 * wrapper in our project lets recovery run before the vendor memory/clock/OS
 * initialization without editing cpu/br23/setup.c. */
extern void sdk_setup_arch(void);

/* Keep this operation opaque to whole-program LTO. The optimizer otherwise
 * hoists wdt_close() ahead of sdk_setup_arch(), which subsequently enables
 * WDT_4S again. */
__attribute__((noinline, optnone))
static void disable_sdk_watchdog(void)
{
    void (*volatile close_watchdog)(void) = wdt_close;
    close_watchdog();
    __asm__ volatile("csync" ::: "memory");
}

/* Temporary startup checkpoint. The system app task invokes early initcalls
 * before the SDK's filesystem/OTP/syscfg sequence and before app_main(). */
static int cyclone2_early_init_checkpoint(void)
{
    disable_sdk_watchdog();
    cyclone2_led_initialize();
    cyclone2_led_set_all(8U, 0U, 0U);
    for (;;) {
        wdt_clear();
        rom_mdelay(100U);
    }
    return 0;
}

early_initcall(cyclone2_early_init_checkpoint);

#define GPIO_PORT_A_BASE         0x001e5000U
#define GPIO_PORT_B_BASE         0x001e5040U
#define GPIO_PORT_C_BASE         0x001e5080U
#define GPIO_OUT(base)           (*(volatile u32 *)((base) + 0x00U))
#define GPIO_IN(base)            (*(volatile u32 *)((base) + 0x04U))
#define GPIO_DIR(base)           (*(volatile u32 *)((base) + 0x08U))
#define GPIO_DIE(base)           (*(volatile u32 *)((base) + 0x0cU))
#define GPIO_PU(base)            (*(volatile u32 *)((base) + 0x10U))
#define GPIO_PD(base)            (*(volatile u32 *)((base) + 0x14U))
#define PIN(number)              (1U << (number))

#define CYCLONE2_PIN_BIT(pin)    (1U << ((pin) & 15U))
#define START_BUTTON_BIT         CYCLONE2_PIN_BIT(CYCLONE2_BUTTON_START)
#define HOME_BUTTON_BIT          CYCLONE2_PIN_BIT(CYCLONE2_BUTTON_HOME)
#define USB_SENSE_BIT            CYCLONE2_PIN_BIT(CYCLONE2_USB_SENSE)

#define USB_CON0                 (*(volatile u32 *)0x001e1800U)
#define USB_CON1                 (*(volatile u32 *)0x001e1804U)
#define USB_IO_CON0              (*(volatile u32 *)0x001e5100U)
#define USB_FORCE_SE0            PIN(11)

#define BOOT_DEBOUNCE_SAMPLES    8U
#define BOOT_DEBOUNCE_MS         20U
#define UBOOT_BOOT_STATE         2U

__attribute__((noreturn))
static void enter_maskrom_uboot(void)
{
    /* Force a real host-visible USB detach before resetting. A CPU reset by
     * itself leaves enough USB state behind that MaskROM often cannot
     * enumerate until the cable is physically removed. */
    USB_CON0 = 0U;
    USB_CON1 = 0U;
    USB_IO_CON0 = USB_FORCE_SE0;
    rom_mdelay(1000U);
    USB_IO_CON0 = 0U;
    rom_mdelay(250U);

    /* Do not commit to UBOOT if VBUS disappeared during the detach window. */
    while ((GPIO_IN(GPIO_PORT_C_BASE) & USB_SENSE_BIT) == 0U) {
        rom_mdelay(100U);
    }

    rom_nvram_set_boot_state(UBOOT_BOOT_STATE);
    rom_chip_reset();
    for (;;) {
    }
}

static u32 early_recovery_condition(void)
{
    u32 sample;

    for (sample = 0; sample < BOOT_DEBOUNCE_SAMPLES; sample++) {
        if ((GPIO_IN(GPIO_PORT_A_BASE) & START_BUTTON_BIT) != 0U ||
            (GPIO_IN(GPIO_PORT_B_BASE) & HOME_BUTTON_BIT) != 0U ||
            (GPIO_IN(GPIO_PORT_C_BASE) & USB_SENSE_BIT) == 0U) {
            return 0U;
        }
        rom_mdelay(BOOT_DEBOUNCE_MS);
    }
    return 1U;
}

static void configure_early_input(u32 base, u32 bit, u32 pull_up)
{
    GPIO_PD(base) &= ~bit;
    if (pull_up) {
        GPIO_PU(base) |= bit;
    } else {
        GPIO_PU(base) &= ~bit;
    }
    GPIO_DIE(base) |= bit;
    GPIO_DIR(base) |= bit;
}

static void cyclone2_early_boot(void)
{
    configure_early_input(GPIO_PORT_A_BASE, START_BUTTON_BIT, 1U);
    configure_early_input(GPIO_PORT_B_BASE, HOME_BUTTON_BIT, 1U);
    configure_early_input(GPIO_PORT_C_BASE, USB_SENSE_BIT, 0U);

    if (early_recovery_condition()) {
        enter_maskrom_uboot();
    }
}

void setup_arch(void)
{
    cyclone2_early_boot();
    sdk_setup_arch();

    /* Bring-up policy: setup_arch() enables a four-second watchdog. Keep it
     * disabled until our minimal scheduler/idle configuration is proven to
     * service it reliably. */
    disable_sdk_watchdog();
}

/* The SDK startup code discovers this table by symbol name. Applications add
 * named tasks here so task_create() can use fixed, auditable resources. */
const struct task_info task_info_table[] = {
    {"app_core",   1, 0, 640, 128},
    {"sys_event",  7, 0, 256,   0},
    {"systimer",   7, 0, 128,   0},
    {"c2_control", 2, 0, 256,   0},
    {0, 0},
};

/* The SDK calls here after loading */
void app_main(void)
{
    /* Bring-up checkpoint: prove that SDK startup reaches app_main without
     * involving application dispatch or dynamic task creation. */
    cyclone2_led_initialize();
    cyclone2_led_set_all(8U, 0U, 0U);
    for (;;) {
        wdt_clear();
        rom_mdelay(100U);
    }
}

static int cyclone2_state_machine(struct application *app,
                                  enum app_state state,
                                  struct intent *intent)
{
    (void)app;

    switch (state) {
    case APP_STA_START:
        if (intent && intent->action == ACTION_CYCLONE2_MAIN) {
            cyclone2_control_start();
        }
        break;
    case APP_STA_CREATE:
    case APP_STA_PAUSE:
    case APP_STA_RESUME:
    case APP_STA_STOP:
    case APP_STA_DESTROY:
        break;
    }
    return 0;
}

static int cyclone2_event_handler(struct application *app,
                                  struct sys_event *event)
{
    (void)app;
    (void)event;
    return 0;
}

static const struct application_operation cyclone2_operations = {
    .state_machine = cyclone2_state_machine,
    .event_handler = cyclone2_event_handler,
};

REGISTER_APPLICATION(cyclone2_application) = {
    .name = CYCLONE2_APP_NAME,
    .action = ACTION_CYCLONE2_MAIN,
    .ops = &cyclone2_operations,
    .state = APP_STA_DESTROY,
};
