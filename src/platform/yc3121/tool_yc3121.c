#include <string.h>
#include "pad.h"
#include "usb_yc3121.h"
#include "board.h"

/* USB on the YC3121 pad: the tool device's commands go to usb.c, as on the
 * other chips; the report's flags (byte 18) say which way the data goes.
 * USB as a game controller is not here yet: a personality asked for leaves
 * the tool device as it is, and nothing is ever ready to take input. */

u32 usb_tool_command(const u8 *report, u8 *data, u32 n, u32 max)
{
    u32 want = (u32)(report[16] | report[17] << 8);

    if (report[18] & 1) {                /* data to the pad */
        usb_command(report, data, n, 1);
        return 0;
    }
    if (want < max) max = want;
    memset(data, 0, max);
    usb_command(report, data, max, 0);
    return max;
}

int plat_usb_present(void)
{
    return board_usb_present();
}

int plat_usb_power(void)
{
    return board_usb_present();
}

int plat_usb_suspended(void) { return 0; }

/* No config mode over LE here: nothing is lent. */
u8 *plat_tool_lend(u32 *size)
{
    (void)size;
    return NULL;
}

void plat_tool_return(void) {}
void plat_usb_probe_request(u32 extra) { (void)extra; }

void plat_usb_mode(const struct usb_identity *id,
                   int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max))
{
    (void)read;
    if (id) util_log("usb: no game controller personality on this pad yet");
}

void plat_usb_audio(u8 sound) { (void)sound; }
int plat_usb_ready(void) { return 0; }
int plat_usb_jack_in(void) { return 0; }
int plat_usb_input_wanted(void) { return 0; }
int plat_usb_input_staged(void) { return 0; }

int plat_usb_send_input(u8 id, const u8 *data, u16 len)
{
    (void)id;
    (void)data;
    (void)len;
    return 0;
}

int plat_usb_send_message(u16 id, const u8 *data, u16 len)
{
    (void)id;
    (void)data;
    (void)len;
    return 0;
}

int plat_usb_take(struct bt_write *w)
{
    (void)w;
    return 0;
}
