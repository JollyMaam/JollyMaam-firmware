#include <system/includes.h>
#include <system/user_cfg.h>

static u8 base_mac_addr[6];
static u8 mac_addr[6];
static u8 edr_name[32] = "CYCLONE2";

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

void cyclone2_bt_address_init(void)
{
    int length = syscfg_read(CFG_BT_MAC_ADDR, base_mac_addr, sizeof(base_mac_addr));

    /* cfg_tool.bin deliberately supplies ff:ff:ff:ff:ff:ff.  Match the
     * vendor SDK: create an address on first boot and retain it in syscfg. */
    if (length != sizeof(base_mac_addr) || address_is(base_mac_addr, 0x00U) ||
        address_is(base_mac_addr, 0xffU)) {
        get_random_number(base_mac_addr, sizeof(base_mac_addr));
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
    /* Locally administered, so it cannot collide with a real vendor address.
     * Addresses are stored least significant byte first, so the first octet
     * anyone prints is index 5. */
    base_mac_addr[5] |= 0x02U;
    syscfg_write(CFG_BT_MAC_ADDR, base_mac_addr, sizeof(base_mac_addr));
    memcpy(mac_addr, base_mac_addr, sizeof(mac_addr));
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
