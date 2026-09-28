#include <system/includes.h>
#include <system/user_cfg.h>

#include "identity.h"
#include "main.h"
#include "usb.h"

/* The address the running session uses; profile.c sets it from
 * config_slot_address() before the stack starts. */
static u8 mac_addr[6];

#define CYCLONE2_LOCAL_NAME_MAX 31U
static u8 edr_name[CYCLONE2_LOCAL_NAME_MAX + 1U] = "CYCLONE2";

/* Link keys are the slots' bonds in config (src/bt/btstack_jl_sdk.c); the
 * vendor host library's ``set_bt_vm_interface()`` database is gone with the
 * vendor stack.  This hook stays because the profile code calls it before it
 * starts a session. */
void cyclone2_remote_db_init(void)
{
}

const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6 },
    {CFG_BT_FRE_OFFSET, 6 },
    {0, 0 },
};

/* Diagnostic hook for the controller-library patch in tools/patch_btctrler.py:
 * called with the link key the stack is about to hand the controller when it
 * answers a peer's authentication request.  Logging it lets us compare what
 * was stored against what is actually used. */
void cyclone2_link_key_seen(const u8 *address, const u8 *key)
{
    cyclone2_usb_debug("LINKKEY peer %02x:%02x:%02x:%02x:%02x:%02x key %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                       address[5], address[4], address[3], address[2],
                       address[1], address[0],
                       key[0], key[1], key[2], key[3], key[4], key[5],
                       key[6], key[7], key[8], key[9], key[10], key[11],
                       key[12], key[13], key[14], key[15]);
}

/* user_cfg.h */
const u8 *bt_get_mac_addr(void)
{
  return mac_addr;
}

/* user_cfg.h */
void bt_set_mac_addr(u8 *addr)
{
  /* The stack stores the address least significant byte first, so print it
   * back to front to match what bluetoothctl and the console display. */
  cyclone2_usb_debug("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", __func__,
                     addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  memcpy(mac_addr, addr, 6);
}

/* user_cfg.h */
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

/* user_cfg.h */
const char *bt_get_local_name()
{
    return (const char *)edr_name;
}
