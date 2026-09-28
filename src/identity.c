#include <system/includes.h>
#include <system/user_cfg.h>

#include "main.h"
#include "usb.h"

static u8 mac_addr[6];

#define CYCLONE2_LOCAL_NAME_MAX 31U
static u8 edr_name[CYCLONE2_LOCAL_NAME_MAX + 1U] = "CYCLONE2";

const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6 },
    {CFG_BT_FRE_OFFSET, 6 },
    {0, 0 },
};

const u8 *bt_get_mac_addr(void)
{
  return mac_addr;
}

void bt_set_mac_addr(u8 *addr)
{
  /* The stack stores the address least significant byte first, so print it
   * back to front to match what bluetoothctl and the console display. */
  cyclone2_usb_debug("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", __func__,
                     addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  memcpy(mac_addr, addr, 6);
}

void bt_set_local_name(char *name, u8 len)
{
  /* At most 31 bytes.  A 34-byte name overran the old 32-byte buffer here,
   * and with the buffer enlarged the pad reset while the controller was
   * starting (2026-09-27, DualSense slot): the Jieli controller library looks
   * to be limited to 31 bytes, whatever the Bluetooth spec allows. */
  if (len > CYCLONE2_LOCAL_NAME_MAX) len = CYCLONE2_LOCAL_NAME_MAX;
  memset(edr_name, 0, sizeof(edr_name));
  memcpy(edr_name, name, len);
}

const char *bt_get_local_name()
{
    return (const char *)edr_name;
}
