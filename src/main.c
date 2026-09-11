#include "typedef.h"
#include "asm/wdt.h"
#include "system/app_core.h"
#include "system/boot.h"
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

#ifndef C2_USE_VENDOR_DEBUG_INIT
/* The vendor implementation only configures the CPU/system debug unit: MMU,
 * invalid-access, write-range, PC-range, stack and arithmetic traps. It is
 * useful with a debug UART, but IRQ 1 repeatedly fires with an empty status
 * register in this standalone image. Keep normal exception-vector setup in
 * setup_arch(), while using release-style operation without those traps. */
void debug_init(void)
{
}
#endif

#ifndef C2_EARLY_RED
#define C2_EARLY_RED 8U
#define C2_EARLY_GREEN 8U
#define C2_EARLY_BLUE 0U
#endif

#ifndef C2_PLATFORM_RED
#define C2_PLATFORM_RED 0U
#define C2_PLATFORM_GREEN 8U
#define C2_PLATFORM_BLUE 0U
#endif

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

static void hold_diagnostic(void)
{
    for (;;) {
        wdt_clear();
        rom_mdelay(100U);
    }
}

/* startup.S passes exception_analyze() a frame containing r0-r15 followed by
 * RETI, RETE, RETX, RETS, PSR, ICFG, USP and SSP. The release implementation
 * in cpu.a discards it and resets; retain the SDK entry assembly but provide a
 * visible analyzer while no debug UART is available. */
__attribute__((noreturn))
void exception_analyze(u32 *saved_registers)
{
    u32 fault_address = saved_registers[16];
    u32 exception_message = q32DSP(0)->EMU_MSG;
    u32 debug_message = JL_DEBUG->MSG;

    /* IRQ 0 reports CPU/EMU faults through EMU_MSG. IRQ 1 reports the
     * system/bus debug unit through JL_DEBUG->MSG. The previous diagnostic
     * proved EMU_MSG was empty, so make the first (long) indication expose
     * the debug-unit family before showing the saved-PC category. */
    if (exception_message & BIT(31)) {
        cyclone2_led_set_all(8U, 0U, 0U); /* red: system exception */
    } else if (exception_message & BIT(30)) {
        cyclone2_led_set_all(8U, 8U, 0U); /* yellow: instruction cache */
    } else if (exception_message & BIT(29)) {
        cyclone2_led_set_all(0U, 8U, 8U); /* cyan: data cache */
    } else if (exception_message & BIT(8)) {
        cyclone2_led_set_all(8U, 0U, 8U); /* magenta: ETM checkpoint */
    } else if (exception_message & 0x001f0000U) {
        cyclone2_led_set_all(0U, 0U, 8U); /* blue: floating point */
    } else if (exception_message != 0U) {
        cyclone2_led_set_all(8U, 8U, 8U); /* white: other CPU/EMU fault */
    } else if (debug_message & (BIT(30) | BIT(29))) {
        cyclone2_led_set_all(8U, 0U, 0U); /* red: cache/SBC RAM access */
    } else if (debug_message & (BIT(28) | BIT(27) | BIT(26))) {
        cyclone2_led_set_all(8U, 8U, 0U); /* yellow: CPU invalid access */
    } else if (debug_message & (BIT(25) | BIT(24) | BIT(23))) {
        cyclone2_led_set_all(0U, 8U, 8U); /* cyan: peripheral invalid access */
    } else if (debug_message & (BIT(22) | BIT(21))) {
        cyclone2_led_set_all(8U, 0U, 8U); /* magenta: CPU range limit */
    } else if (debug_message & (BIT(20) | BIT(19))) {
        cyclone2_led_set_all(0U, 0U, 8U); /* blue: MMU access error */
    } else if (debug_message & BIT(18)) {
        cyclone2_led_set_all(0U, 8U, 0U); /* green: peripheral write limit */
    } else if (debug_message & BIT(17)) {
        cyclone2_led_set_all(8U, 8U, 8U); /* white: watchdog */
    } else if (debug_message != 0U) {
        cyclone2_led_set_all(8U, 0U, 8U); /* magenta: other debug message */
    } else {
        cyclone2_led_set_all(8U, 8U, 8U); /* white: other/empty message */
    }
    rom_mdelay(1500U);
    cyclone2_led_set_all(0U, 0U, 0U);
    rom_mdelay(250U);

    if (fault_address < 0x0000171cU) {
        cyclone2_led_set_all(8U, 0U, 0U); /* red: SFC/flash/power RAM code */
    } else if (fault_address < 0x00001764U) {
        cyclone2_led_set_all(0U, 8U, 8U); /* cyan: interrupt entry code */
    } else if (fault_address < 0x00001c4cU) {
        cyclone2_led_set_all(8U, 0U, 8U); /* magenta: allocator/MMU/context */
    } else if (fault_address < 0x00001f1aU) {
        cyclone2_led_set_all(8U, 0U, 0U); /* red: task/list primitives */
    } else if (fault_address < 0x00002108U) {
        cyclone2_led_set_all(8U, 8U, 0U); /* yellow: queue send/mutex post */
    } else if (fault_address < 0x00002212U) {
        cyclone2_led_set_all(8U, 0U, 0U); /* red: xTaskIncrementTick */
    } else if (fault_address < 0x00002304U) {
        cyclone2_led_set_all(8U, 8U, 0U); /* yellow: xTaskResumeAll */
    } else if (fault_address < 0x0000236cU) {
        cyclone2_led_set_all(0U, 8U, 8U); /* cyan: prvUnlockQueue */
    } else if (fault_address < 0x00002374U) {
        cyclone2_led_set_all(8U, 0U, 0U); /* red: event-list entry/check */
    } else if (fault_address < 0x00002386U) {
        cyclone2_led_set_all(8U, 8U, 0U); /* yellow: CurrentTCB access */
    } else if (fault_address < 0x00002390U) {
        cyclone2_led_set_all(0U, 8U, 8U); /* cyan: vListInsert boundary */
    } else if (fault_address < 0x00002398U) {
        cyclone2_led_set_all(8U, 0U, 8U); /* magenta: delayed-list boundary */
    } else if (fault_address < 0x000023a8U) {
        cyclone2_led_set_all(0U, 0U, 8U); /* blue: assertion path */
    } else if (fault_address < 0x00002444U) {
        cyclone2_led_set_all(0U, 0U, 8U); /* blue: timeout check */
    } else if (fault_address < 0x0000256aU) {
        cyclone2_led_set_all(8U, 0U, 8U); /* magenta: switch/event removal */
    } else if (fault_address < 0x00002886U) {
        cyclone2_led_set_all(0U, 0U, 8U); /* blue: queue receive/mutex pend */
    } else if (fault_address < 0x00002ee2U) {
        cyclone2_led_set_all(8U, 8U, 8U); /* white: OS creation/wrappers */
    } else if (fault_address < 0x0000355cU) {
        cyclone2_led_set_all(0U, 8U, 0U); /* green: idle/ISR/GPIO code */
    } else if (fault_address >= 0x01000000U) {
        cyclone2_led_set_all(8U, 8U, 0U); /* yellow: other mapped flash */
    } else {
        cyclone2_led_set_all(8U, 8U, 8U); /* white: non-code/unknown */
    }
    hold_diagnostic();
}

/* First project checkpoint in the stock initcall dispatcher. */
static int cyclone2_early_init_checkpoint(void)
{
    disable_sdk_watchdog();
    cyclone2_led_initialize();
    cyclone2_led_set_all(C2_EARLY_RED, C2_EARLY_GREEN, C2_EARLY_BLUE);
    return 0;
}

/* Reaching this entry proves all retained early initcalls returned under the
 * stock sequencing. Runtime VFS is deliberately disabled for this controller. */
static int cyclone2_platform_init_checkpoint(void)
{
    /* Earlier SDK initcalls may change a clock, SPI or DMA register after the
     * yellow early marker was latched. Re-establish the LED transport so a
     * retained yellow frame cannot masquerade as an early-init hang. */
    cyclone2_led_initialize();

    /* Blink rather than hold so success cannot be confused with the solid
     * green exception-PC category. */
    for (;;) {
        cyclone2_led_set_all(C2_PLATFORM_RED, C2_PLATFORM_GREEN,
                             C2_PLATFORM_BLUE);
        rom_mdelay(250U);
        cyclone2_led_set_all(0U, 0U, 0U);
        rom_mdelay(250U);
        wdt_clear();
    }
    return 0;
}

early_initcall(cyclone2_early_init_checkpoint);
platform_initcall(cyclone2_platform_init_checkpoint);

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
