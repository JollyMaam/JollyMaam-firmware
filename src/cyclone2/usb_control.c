#include "system/includes.h"
#include "asm/irq.h"
#include "asm/wdt.h"
#include "usb/device/cdc.h"
#include "usb/device/usb_stack.h"

#include "board.h"
#include "control.h"
#include "cyclone2/reset_mailbox.h"
#include "cyclone2/usb_control.h"
#include "rom.h"

#define USB_CON0              (*(volatile u32 *)0x001e1800U)
#define USB_CON1              (*(volatile u32 *)0x001e1804U)
#define USB_IO_CON0           (*(volatile u32 *)0x001e5100U)
#define USB_FORCE_SE0         BIT(11)
#define MASKROM_NVRAM_ADDRESS ((volatile u8 *)0x00010800U)
#define COMMAND_LINE_SIZE     80U
#define RESPONSE_DELAY_TICKS  30U

static char command_line[COMMAND_LINE_SIZE];
static u8 command_length;
static u8 last_was_cr;
static volatile u8 receive_pending;
static u8 greeting_pending;
static u8 uboot_pending;
static u8 response_delay;

extern void usb_start(void);
extern void nvram_set_boot_state(u32 state);
extern void flushinv_dcache(const void *address, u32 size);

static void console_write_bytes(const void *data, u32 length)
{
    cdc_write_data(USB0, (u8 *)data, length);
}

static void console_write(const char *text)
{
    console_write_bytes(text, strlen(text));
}

static void console_write_hex(const u8 *data, u32 length)
{
    static const char digits[] = "0123456789abcdef";
    char encoded[32];
    u32 count;
    u32 index;

    while (length != 0U) {
        count = length > sizeof(encoded) / 2U ? sizeof(encoded) / 2U : length;
        for (index = 0; index < count; index++) {
            encoded[index * 2U] = digits[data[index] >> 4];
            encoded[index * 2U + 1U] = digits[data[index] & 0x0fU];
        }
        console_write_bytes(encoded, count * 2U);
        data += count;
        length -= count;
    }
}

static void console_prompt(void)
{
    console_write("c2> ");
}

static void arm_maskrom_usb_token(void)
{
    nvram_set_boot_state(2U);
    __asm__ volatile("csync");
    flushinv_dcache((const void *)MASKROM_NVRAM_ADDRESS, 16U);
}

static void print_status(void)
{
    const u8 *mailbox = (const u8 *)&cyclone2_boot_request;

    console_write("reset-mailbox=");
    console_write_hex(mailbox, sizeof(cyclone2_boot_request));
    console_write(" nvram=");
    console_write_hex((const u8 *)MASKROM_NVRAM_ADDRESS, 16U);
    console_write("\r\n");
}

static void execute_command(void)
{
    command_line[command_length] = '\0';

    if (!strcmp(command_line, "help") || !strcmp(command_line, "?")) {
        console_write("help     show this command list\r\n"
                      "status   show reset mailbox and MaskROM NVRAM\r\n"
                      "uboot    reboot into BR23 UBOOT1.00\r\n");
    } else if (!strcmp(command_line, "status")) {
        print_status();
    } else if (!strcmp(command_line, "uboot")) {
        arm_maskrom_usb_token();
        console_write("OK entering UBOOT1.00 token=");
        console_write_hex((const u8 *)MASKROM_NVRAM_ADDRESS, 16U);
        console_write("\r\n");
        response_delay = RESPONSE_DELAY_TICKS;
        uboot_pending = 1U;
    } else if (command_length != 0U) {
        console_write("ERR unknown command; type help\r\n");
    }

    command_length = 0U;
    if (!uboot_pending) console_prompt();
}

static void consume_input(const u8 *data, u32 length)
{
    u32 index;

    for (index = 0; index < length; index++) {
        u8 byte = data[index];

        if (byte == '\r' || byte == '\n') {
            if (byte == '\n' && last_was_cr) {
                last_was_cr = 0U;
                continue;
            }
            last_was_cr = byte == '\r';
            console_write("\r\n");
            execute_command();
            if (uboot_pending) return;
        } else if (byte == 0x08U || byte == 0x7fU) {
            last_was_cr = 0U;
            if (command_length != 0U) {
                command_length--;
                console_write("\b \b");
            }
        } else if (byte >= 0x20U && byte <= 0x7eU) {
            last_was_cr = 0U;
            if (command_length + 1U < sizeof(command_line)) {
                command_line[command_length++] = (char)byte;
                console_write_bytes(&byte, 1U);
            }
        }
    }
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

void cyclone2_usb_control_initialize(void)
{
    usb_start();
    cdc_set_wakeup_handler(cdc_receive_wakeup);
    greeting_pending = 1U;
}

void cyclone2_usb_control_poll(void)
{
    u8 receive_buffer[64];
    u32 length;

    if (greeting_pending) {
        static const char greeting[] =
            "\r\nCyclone 2 configuration console\r\nType help for commands.\r\nc2> ";
        if (cdc_write_data(USB0, (u8 *)greeting, sizeof(greeting) - 1U) != 0U)
            greeting_pending = 0U;
    }

    if (receive_pending && !uboot_pending) {
        receive_pending = 0U;
        do {
            length = cdc_read_data(USB0, receive_buffer,
                                   sizeof(receive_buffer));
            if (length != 0U) consume_input(receive_buffer, length);
        } while (length != 0U && !uboot_pending);
    }

    if (uboot_pending && --response_delay == 0U) enter_maskrom_uboot();
}
