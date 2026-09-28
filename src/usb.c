#include <system/includes.h>
#include <asm/irq.h>
#include <asm/wdt.h>

#include "console.h"
#include "controller.h"
#include "led.h"
#include "main.h"
#include "rom.h"
#include "input.h"
#include "usb.h"
#include "usb/device/cdc.h"
#include "usb/device/usb_stack.h"

#define USB_CON0              (*(volatile u32 *)0x001e1800U)
#define USB_CON1              (*(volatile u32 *)0x001e1804U)
#define USB_IO_CON0           (*(volatile u32 *)0x001e5100U)
#define USB_FORCE_SE0         BIT(11)
#define MASKROM_NVRAM_ADDRESS ((volatile u8 *)0x00010800U)
#define RESPONSE_DELAY_TICKS  30U
#define UBOOT_RADIO_TIMEOUT_MS 5000U

static volatile u8 receive_pending;
static u8 uboot_pending;
static u8 response_delay;
static u32 uboot_started_ms;
static u8 terminal_was_open;

extern void usb_start(void);
extern void nvram_set_boot_state(u32 state);
extern void flushinv_dcache(const void *address, u32 size);
extern u8 cyclone2_cdc_terminal_open(void);

void cyclone2_usb_write(const void *data, u32 length)
{
    cdc_write_data(USB0, (u8 *)data, length);
}

int cyclone2_usb_uboot_pending(void)
{
    return uboot_pending;
}

static void arm_maskrom_usb_token(void)
{
    nvram_set_boot_state(2U);
    __asm__ volatile("csync");
    flushinv_dcache((const void *)MASKROM_NVRAM_ADDRESS, 16U);
}

void cyclone2_usb_uboot_request(void)
{

    if (cyclone2_disconnect_request() > 0) console_write("OK disconnecting first\r\n");
    uboot_started_ms = sys_timer_get_ms();
    arm_maskrom_usb_token();
    console_write("OK entering UBOOT1.00 token=");
    console_hex((const u8 *)MASKROM_NVRAM_ADDRESS, 16U);
    console_write("\r\n");
    response_delay = RESPONSE_DELAY_TICKS;
    uboot_pending = 1U;
}

static void cdc_receive_wakeup(struct usb_device_t *usb_device)
{
    (void)usb_device;
    receive_pending = 1U;
}

__attribute__((noreturn))
static void enter_maskrom_uboot(void)
{
    cyclone2_led_set_all(0U, 16U, 0U);
    rom_mdelay(100U);

    local_irq_disable();
    USB_CON0 = 0U;
    USB_CON1 = 0U;
    USB_IO_CON0 = USB_FORCE_SE0;
    rom_mdelay(1000U);
    USB_IO_CON0 = 0U;
    rom_mdelay(250U);

    while (!gpio_read(CYCLONE2_USB_SENSE)) rom_mdelay(100U);

    arm_maskrom_usb_token();
    wdt_init(WDT_256MS);
    local_irq_disable();
    for (;;) {
    }
}

static void uboot_step(void)
{
    static u8 warned;

    if (!cyclone2_radio_stopped()) {
        if (sys_timer_get_ms() - uboot_started_ms < UBOOT_RADIO_TIMEOUT_MS)
            return;
        if (!warned) {

            warned = 1U;
            cyclone2_bringup_record(0x5e00u);

            console_write("WARN radio still up after 5 s; entering UBOOT anyway\r\n");
        }
    }
    if (--response_delay == 0U) enter_maskrom_uboot();
}

void cyclone2_usb_control_initialize(void)
{
    usb_start();
    cdc_set_wakeup_handler(cdc_receive_wakeup);
}

void cyclone2_usb_control_poll(void)
{
    u8 receive_buffer[64];
    u32 length;

    if (!cyclone2_cdc_terminal_open()) {
        if (terminal_was_open) console_closed();
        terminal_was_open = 0U;
        if (uboot_pending) uboot_step();
        return;
    }
    if (!terminal_was_open) {
        terminal_was_open = 1U;
        console_opened();
    }
    if (receive_pending && !uboot_pending) {
        receive_pending = 0U;
        do {
            length = cdc_read_data(USB0, receive_buffer, sizeof(receive_buffer));
            if (length != 0U) console_input(receive_buffer, length);
        } while (length != 0U && !uboot_pending);
    }
    if (!uboot_pending) console_poll();
    if (uboot_pending) uboot_step();
}
