#include "system/includes.h"
#include "asm/irq.h"
#include "asm/wdt.h"
#include "usb/device/custom_hid.h"
#include "usb/device/usb_stack.h"

#include "board.h"
#include "control.h"
#include "cyclone2/reset_mailbox.h"
#include "cyclone2/usb_control.h"
#include "cyclone2/usb_protocol.h"
#include "rom.h"

#define USB_CON0                (*(volatile u32 *)0x001e1800U)
#define USB_CON1                (*(volatile u32 *)0x001e1804U)
#define USB_IO_CON0             (*(volatile u32 *)0x001e5100U)
#define USB_FORCE_SE0           BIT(11)
#define RESPONSE_DELAY_TICKS   30U

enum usb_command_state {
    USB_COMMAND_IDLE,
    USB_COMMAND_RECEIVED,
    USB_COMMAND_ACKNOWLEDGED,
};

static const u8 protocol_magic[CYCLONE2_USB_MAGIC_SIZE] =
    CYCLONE2_USB_MAGIC_INITIALIZER;
static u8 response[CYCLONE2_USB_PACKET_SIZE];
static volatile u8 command_state;
static volatile u8 received_command;
static u8 response_delay;

extern void usb_start(void);
extern void nvram_set_boot_state(u32 state);
extern void flushinv_dcache(const void *address, u32 size);

#define MASKROM_NVRAM_ADDRESS ((volatile u8 *)0x00010800U)

static void arm_maskrom_usb_token(void)
{
    nvram_set_boot_state(2U);
    __asm__ volatile("csync");
    flushinv_dcache((const void *)MASKROM_NVRAM_ADDRESS, 16U);
}

static void receive_command(void *private_data, u8 *packet, u32 length)
{
    u8 command;
    (void)private_data;

    /* A UBOOT request remains pending briefly so its ACK can reach the host.
     * Do not let a follow-up STATUS packet replace it during that window. */
    if (command_state != USB_COMMAND_IDLE) return;

    if (length != CYCLONE2_USB_PACKET_SIZE ||
        memcmp(packet, protocol_magic, sizeof(protocol_magic))) return;

    command = packet[CYCLONE2_USB_COMMAND_OFFSET];
    if (packet[CYCLONE2_USB_INVERSE_OFFSET] != (u8)~command ||
        (command != CYCLONE2_USB_COMMAND_UBOOT &&
         command != CYCLONE2_USB_COMMAND_STATUS)) return;

    /* This callback runs in USB interrupt context. Defer the response, LED,
     * delays, and reset sequence to the application's periodic task. */
    received_command = command;
    command_state = USB_COMMAND_RECEIVED;
}

__attribute__((noreturn))
static void enter_maskrom_uboot(void)
{
    cyclone2_led_set_all(0U, 16U, 0U);
    rom_mdelay(100U);

    /* This exact forced-SE0 sequence was validated on the Cyclone 2. Merely
     * resetting the CPU leaves enough USB state behind that MaskROM may not
     * enumerate until the cable is physically removed. */
    local_irq_disable();
    USB_CON0 = 0U;
    USB_CON1 = 0U;
    USB_IO_CON0 = USB_FORCE_SE0;
    rom_mdelay(1000U);
    USB_IO_CON0 = 0U;
    rom_mdelay(250U);

    /* The command arrived over USB, but do not commit to UBOOT if the cable
     * was removed during the deliberate detach interval. */
    while (!gpio_read(CYCLONE2_USB_SENSE)) rom_mdelay(100U);

    /* Rewrite immediately before reset in case the SDK heap reused 0x10800
     * during the deliberate USB-detach delay. A watchdog expiration is a
     * genuinely different reset source from both tested software resets. */
    arm_maskrom_usb_token();
    wdt_init(WDT_256MS);
    local_irq_disable();
    for (;;) {
    }
}

void cyclone2_usb_control_initialize(void)
{
    usb_start();
    custom_hid_set_rx_hook(NULL, receive_command);
}

void cyclone2_usb_control_poll(void)
{
    u8 command;

    if (command_state == USB_COMMAND_RECEIVED) {
        memset(response, 0, sizeof(response));
        memcpy(response, protocol_magic, sizeof(protocol_magic));
        command = received_command | CYCLONE2_USB_RESPONSE_BIT;
        response[CYCLONE2_USB_COMMAND_OFFSET] = command;
        response[CYCLONE2_USB_INVERSE_OFFSET] = (u8)~command;
        if (received_command == CYCLONE2_USB_COMMAND_UBOOT) {
            /* Arm before acknowledging so c2ctl can prove the exact bytes
             * written by the ROM helper, before any reset occurs. */
            arm_maskrom_usb_token();
            memcpy(response + CYCLONE2_USB_DATA_OFFSET,
                   (const void *)MASKROM_NVRAM_ADDRESS, 16U);
        }
        if (received_command == CYCLONE2_USB_COMMAND_STATUS) {
            memcpy(response + CYCLONE2_USB_DATA_OFFSET,
                   (const void *)&cyclone2_boot_request,
                   sizeof(cyclone2_boot_request));
            memcpy(response + CYCLONE2_USB_DATA_OFFSET +
                       sizeof(cyclone2_boot_request),
                   (const void *)MASKROM_NVRAM_ADDRESS, 16U);
        }
        custom_hid_tx_data(0, response, sizeof(response));
        if (received_command == CYCLONE2_USB_COMMAND_STATUS) {
            command_state = USB_COMMAND_IDLE;
            return;
        }
        response_delay = RESPONSE_DELAY_TICKS;
        command_state = USB_COMMAND_ACKNOWLEDGED;
    } else if (command_state == USB_COMMAND_ACKNOWLEDGED &&
               --response_delay == 0U) {
        enter_maskrom_uboot();
    }
}
