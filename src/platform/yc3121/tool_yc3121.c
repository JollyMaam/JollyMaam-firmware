#include <string.h>
#include "pad.h"
#include "usb_yc3121.h"
#include "board.h"

/* USB on the YC3121 pad: the tool device's commands go to usb.c, as on the
 * other chips; the report's flags (byte 18) say which way the data goes.
 * The personalities are usb_yc3121.c's; no sound card here. */

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

void plat_usb_audio(u8 sound) { (void)sound; }
int plat_usb_jack_in(void) { return 0; }
