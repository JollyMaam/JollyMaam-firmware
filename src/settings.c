#include <system/includes.h>
#include <system/user_cfg.h>

#include "usb.h"

static u8 base_mac_addr[6];
static u8 mac_addr[6];
static u8 edr_name[32] = "CYCLONE2";

/* The Bluetooth stack keeps its remote-device database - one 38-byte record per
 * peer with the peer address and the 16-byte link key - behind
 * ``set_bt_vm_interface()`` and a small vtable.  Nothing in the SDK calls it,
 * and without it `db_file_read()`/`db_file_write()` silently do nothing
 * (`dbf_bt_rw_file` stays NULL), so a paired link key is never written and a
 * later reconnect asks for it, gets "PIN or key missing", and is turned away.
 *
 * The syscfg ids for that database are reserved by the SDK itself:
 * CFG_REMOTE_DB_INFO (61) followed by CFG_REMOTE_DB_00..19 (62..81), which is
 * exactly the space the database needs (16 device records plus two small
 * tables).  The vtable layout is fixed by database_file.c.o: open(index, len),
 * read(buf, len), write(buf, len), seek(offset, whence), close().  Each 38-byte
 * record is one syscfg item, so read/write map straight onto syscfg_read() and
 * syscfg_write(), and the only seek the database performs is seek(0, 0). */
struct remote_db_file {
    int (*open)(u32 index, u32 len);
    int (*read)(u8 *buffer, u32 len);
    int (*write)(u8 *buffer, u32 len);
    int (*seek)(u32 offset, u32 whence);
    int (*close)(void);
};

static u32 remote_db_id;
static u8 remote_db_trace;

static int remote_db_open(u32 index, u32 len)
{
    (void)len;
    remote_db_id = index;
    if (remote_db_trace != 0U) {
        remote_db_trace--;
        cyclone2_usb_debug("remote db open id=%u", index);
    }
    return 1;
}

static int remote_db_read(u8 *buffer, u32 len)
{
    int result = syscfg_read(remote_db_id, buffer, len);
    if (remote_db_trace != 0U) {
        remote_db_trace--;
        cyclone2_usb_debug("remote db read id=%u len=%u result=%d",
                           remote_db_id, len, result);
    }
    return result == (int)len;
}

static int remote_db_write(u8 *buffer, u32 len)
{
    int result = syscfg_write(remote_db_id, buffer, len);
    if (remote_db_trace != 0U) {
        remote_db_trace--;
        cyclone2_usb_debug("remote db write id=%u len=%u result=%d",
                           remote_db_id, len, result);
    }
    return result == (int)len;
}

static int remote_db_seek(u32 offset, u32 whence)
{
    (void)offset;
    (void)whence;
    return 1;
}

static int remote_db_close(void)
{
    return 1;
}

static const struct remote_db_file remote_db_interface = {
    remote_db_open,
    remote_db_read,
    remote_db_write,
    remote_db_seek,
    remote_db_close,
};

extern void set_bt_vm_interface(u32 vm_index, void *interface);

void cyclone2_remote_db_init(void)
{
    remote_db_trace = 24U;
    set_bt_vm_interface(CFG_REMOTE_DB_INFO,
                        (void *)&remote_db_interface);
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
