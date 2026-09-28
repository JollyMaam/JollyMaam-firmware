/*
 * Console sink for BTstack's own trace.
 *
 * BTstack calls hci_dump_packet() for every HCI command, event and ACL packet,
 * and hci_dump_log() for its log_info/log_error messages, but a build with no
 * hci_dump implementation registered throws both away.  That is why a live
 * pairing used to be visible only through the application's own trace points,
 * which do not carry the fields that decide the outcome.
 *
 * With this sink the console shows the whole exchange:
 *
 *   - the IO Capability Request/Response, i.e. both sides' authentication
 *     requirements - the "no bonding" bit that decides whether a derived link
 *     key may be stored at all;
 *   - the Link Key Notification, with the 16-byte key the controller derived;
 *   - on a reconnect, the Link Key Request and the reply the host answers
 *     with, which is where a missing or stale bond shows up.
 *
 * The lines are ordinary log output (config log.console).  Messages follow
 * config log.btstack and packets config log.hcidump, read as they arrive, so
 * turning either on or off takes effect at once; packets are off by default
 * because a connected session makes far more of them than the log can hold.
 */
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
    /* "<" is controller to host, ">" host to controller. */
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
    /* Re-registering is cheap and happens on every profile start.  BTstack
     * always hands packets over; config log.hcidump decides here whether
     * they are printed, so the switch works mid-session. */
    hci_dump_init(&dump_console);
    hci_dump_enable_packet_log(true);
}
