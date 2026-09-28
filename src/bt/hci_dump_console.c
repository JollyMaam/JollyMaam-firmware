#include <system/includes.h>

#include "hci_dump.h"
#include "config.h"
#include "usb.h"

#include "hci_dump_console.h"

/* Bytes of a packet the trace keeps.  Enough for a Link Key Notification
 * (header plus 22 bytes) and every other pairing event, without dragging a
 * whole ACL payload through a 96-byte debug line. */
#define HCI_DUMP_BYTES 32U

static void dump_reset(void)
{
}

static void dump_packet(uint8_t packet_type, uint8_t in, uint8_t *packet,
                        uint16_t len)
{
    static const char digits[] = "0123456789abcdef";
    char hex[HCI_DUMP_BYTES * 2U + 1U];
    unsigned count;
    unsigned index;

    if (!config.log.hcidump || len == 0U) return;

    count = len > HCI_DUMP_BYTES ? HCI_DUMP_BYTES : len;
    for (index = 0U; index < count; index++) {
        hex[index * 2U] = digits[packet[index] >> 4];
        hex[index * 2U + 1U] = digits[packet[index] & 0x0fU];
    }
    hex[count * 2U] = '\0';

    cyclone2_usb_debug("HCI t%u %s %s", (unsigned)packet_type,
                       in ? "<" : ">", hex);
}

static void dump_message(int log_level, const char *format, va_list argptr)
{
    char text[64];

    if (!config.log.btstack) return;
    vsnprintf(text, sizeof(text), format, argptr);
    cyclone2_usb_debug("BT%u %s", (unsigned)log_level, text);
}

static const hci_dump_t dump_console = {
    .reset = &dump_reset,
    .log_packet = &dump_packet,
    .log_message = &dump_message,
};

void cyclone2_hci_dump_init(void)
{

    hci_dump_init(&dump_console);
    hci_dump_enable_packet_log(true);
}
