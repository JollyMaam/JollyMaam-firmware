#include <system/includes.h>
#include <system/user_cfg.h>

#include "usb.h"

static u8 base_mac_addr[6];
static u8 mac_addr[6];
static u8 edr_name[32] = "CYCLONE2";

/* The BTstack port keeps its link keys in CFG_REMOTE_DB_00.. itself
 * (src/bt/btstack_jl_sdk.c); the vendor host library's
 * ``set_bt_vm_interface()`` database is gone with the vendor stack.  This hook
 * stays because the profile code calls it before it starts a session. */
void cyclone2_remote_db_init(void)
{
}

const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6 },
    {CFG_BT_FRE_OFFSET, 6 },
    {0, 0 },
};

static int address_is(const u8 address[6], u8 value)
{
    unsigned i;

    for (i = 0; i < 6U; ++i) {
        if (address[i] != value) return 0;
    }
    return 1;
}

/* Keep a generated address usable as a device address.  The low bit of the
 * most significant octet is the group bit (0 = individual address) and the bit
 * above it marks a locally administered address.  Addresses are stored least
 * significant byte first, so the octet anyone prints first is index 5.  A
 * random draw has a 50% chance of setting the group bit, and a peer that takes
 * the address seriously will not talk to a group address. */
static void normalize_address(u8 address[6])
{
    address[5] = (u8)((address[5] | 0x02U) & 0xfeU);
}

void cyclone2_bt_address_init(void)
{
    int length = syscfg_read(CFG_BT_MAC_ADDR, base_mac_addr, sizeof(base_mac_addr));

    /* cfg_tool.bin deliberately supplies ff:ff:ff:ff:ff:ff.  Match the
     * vendor SDK: create an address on first boot and retain it in syscfg. */
    if (length != sizeof(base_mac_addr) || address_is(base_mac_addr, 0x00U) ||
        address_is(base_mac_addr, 0xffU)) {
        get_random_number(base_mac_addr, sizeof(base_mac_addr));
        normalize_address(base_mac_addr);
        syscfg_write(CFG_BT_MAC_ADDR, base_mac_addr, sizeof(base_mac_addr));
    }
    memcpy(mac_addr, base_mac_addr, sizeof(mac_addr));
}

void cyclone2_bt_address_for_slot(u8 slot, u8 address[6])
{
    memcpy(address, base_mac_addr, sizeof(base_mac_addr));

    /* Addresses are stored least-significant byte first.  Keep the upper
     * five bytes stable and allocate four deterministic addresses from the
     * persisted device base. */
    address[0] ^= (u8)((slot & 3U) * 4U);
}

/* Give this controller a brand-new Bluetooth identity.
 *
 * A host keeps its link key against the controller's address, so when the two
 * sides disagree about a key, deleting only one side's copy cannot recover the
 * pairing: the host keeps challenging the old address and the connection ends
 * with "authentication failure".  Changing the address makes this look like a
 * controller the host has never seen, which pairs from scratch.  The vendor
 * SDK's own unpair path regenerates addresses for the same reason. */
void cyclone2_bt_address_randomize(void)
{
    get_random_number(base_mac_addr, sizeof(base_mac_addr));
    normalize_address(base_mac_addr);
    syscfg_write(CFG_BT_MAC_ADDR, base_mac_addr, sizeof(base_mac_addr));
    memcpy(mac_addr, base_mac_addr, sizeof(mac_addr));
}

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
  memset(edr_name, 0, sizeof(edr_name));
  memcpy(edr_name, name, len);
  edr_name[31] = 0;
}

/* user_cfg.h */
const char *bt_get_local_name()
{
    return (const char *)edr_name;
}
