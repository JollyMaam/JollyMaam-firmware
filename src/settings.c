#include <system/includes.h>
#include <system/user_cfg.h>

#include "main.h"
#include "settings.h"
#include "usb.h"

static u8 base_mac_addr[6];
static u8 base_mac_addr_dirty;
static u8 mac_addr[6];

/* ------------------------------------------------------- where it is kept */

/* Everything this firmware persists - the pad's Bluetooth address, each
 * slot's peer and Switch 2 bond here, the link keys (btstack_jl_sdk.c), the
 * profile settings (controller.c) and the calibration (calibration.c) - is
 * held in RAM and written to syscfg from there at safe points.  RAM mode
 * holds those writes back, so a configuration can be tried without touching
 * flash; see settings.h. */
static u8 store_ram_mode;

int cyclone2_store_ram_mode(void)
{
    return store_ram_mode;
}

void cyclone2_store_set_ram_mode(int ram)
{
    store_ram_mode = ram ? 1U : 0U;
}
#define CYCLONE2_LOCAL_NAME_MAX 31U
static u8 edr_name[CYCLONE2_LOCAL_NAME_MAX + 1U] = "CYCLONE2";

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

    base_mac_addr_dirty = 0U;
    /* cfg_tool.bin deliberately supplies ff:ff:ff:ff:ff:ff.  Match the
     * vendor SDK: create an address on first boot and retain it in syscfg
     * (at the next flush). */
    if (length != sizeof(base_mac_addr) || address_is(base_mac_addr, 0x00U) ||
        address_is(base_mac_addr, 0xffU)) {
        get_random_number(base_mac_addr, sizeof(base_mac_addr));
        normalize_address(base_mac_addr);
        base_mac_addr_dirty = 1U;
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
    base_mac_addr_dirty = 1U;
    memcpy(mac_addr, base_mac_addr, sizeof(mac_addr));
}

/* Put back an address that was saved off the pad.  A bond is keyed to the
 * address the host saw, so restoring a captured pairing has to restore the
 * address it was captured under as well. */
void cyclone2_bt_address_set(const u8 address[6])
{
    /* Verbatim: normalize_address() is for addresses we make up.  An address
     * being restored - or a public one, like the stock firmware's
     * D0:56:80:.. - must stay exactly as given. */
    memcpy(base_mac_addr, address, sizeof(base_mac_addr));
    base_mac_addr_dirty = 1U;
    memcpy(mac_addr, base_mac_addr, sizeof(mac_addr));
}

/* The console a slot last connected to, i.e. the peer the slot pages when it
 * starts on its own.  Together with the link key in the host's key database
 * (src/bt/btstack_jl_sdk.c) this is the slot's bond; the address alone is not
 * enough to authenticate, and the key alone does not say who to page.  A slot
 * that has never been written reads back as all zeroes: no peer to page.
 *
 * Cached in RAM and written back by cyclone2_settings_flush() once the radio
 * is idle.  It used to be written straight through, and one of those writes
 * is on the pairing path: starting a slot in pairing mode clears its peer.  A
 * syscfg write there resets the pad - measured: `pair N` restarted the chip
 * seconds later, every time, while the same write on an idle pad is fine. */
static u8 peer_address_cache[CFG_BT_IDX_NUM][6];
static u8 peer_address_dirty[CFG_BT_IDX_NUM];

int cyclone2_peer_address_read(u8 slot, u8 address[6])
{
    if (slot >= CFG_BT_IDX_NUM) return 0;
    memcpy(address, peer_address_cache[slot], 6U);
    return 1;
}

void cyclone2_peer_address_write(u8 slot, const u8 address[6])
{
    if (slot >= CFG_BT_IDX_NUM) return;
    memcpy(peer_address_cache[slot], address, 6U);
    peer_address_dirty[slot] = 1U;
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

/* ------------------------------------------------------ Switch 2 bonds */

/* One record per slot in the vendor's (unused) LE bonding items,
 * CFG_BLE_BONDING_REMOTE_INFO + slot: tag, console address, LTK. */
#define LE_BOND_TAG   0xB2U
#define LE_BOND_SIZE  23U

static u8 le_bond[CFG_BT_IDX_NUM][LE_BOND_SIZE];
static u8 le_bond_dirty[CFG_BT_IDX_NUM];

int cyclone2_le_bond_read(u8 slot, u8 console[6], u8 ltk[16])
{
    if (slot >= CFG_BT_IDX_NUM) return 0;
    if (le_bond[slot][0] != LE_BOND_TAG) return 0;
    memcpy(console, &le_bond[slot][1], 6U);
    memcpy(ltk, &le_bond[slot][7], 16U);
    return 1;
}

void cyclone2_le_bond_write(u8 slot, const u8 console[6], const u8 ltk[16])
{
    if (slot >= CFG_BT_IDX_NUM) return;
    le_bond[slot][0] = LE_BOND_TAG;
    memcpy(&le_bond[slot][1], console, 6U);
    memcpy(&le_bond[slot][7], ltk, 16U);
    le_bond_dirty[slot] = 1U;
}

void cyclone2_le_bond_clear(u8 slot)
{
    if (slot >= CFG_BT_IDX_NUM) return;
    if (le_bond[slot][0] == 0U) return;
    memset(le_bond[slot], 0, LE_BOND_SIZE);
    le_bond_dirty[slot] = 1U;
}

/* ------------------------------------------------ loading and writing */

/* Read the address, peers and Switch 2 bonds from syscfg: at boot, before the
 * radio, and for `store revert` (radio idle).  Pending changes are dropped. */
void cyclone2_settings_load(void)
{
    u8 slot;

    cyclone2_bt_address_init();
    for (slot = 0U; slot < CFG_BT_IDX_NUM; slot++) {
        cyclone2_bringup_record(0x3f00u | slot);
        if (syscfg_read((u16)(CFG_EDR_ADDRESS_BEGIN + slot),
                        peer_address_cache[slot], 6U) != 6) {
            memset(peer_address_cache[slot], 0, 6U);
        }
        peer_address_dirty[slot] = 0U;
        if (syscfg_read((u16)(CFG_BLE_BONDING_REMOTE_INFO + slot), le_bond[slot],
                        LE_BOND_SIZE) != LE_BOND_SIZE) {
            memset(le_bond[slot], 0, LE_BOND_SIZE);
        }
        le_bond_dirty[slot] = 0U;
    }
}

int cyclone2_settings_pending(void)
{
    u8 slot;

    if (base_mac_addr_dirty) return 1;
    for (slot = 0U; slot < CFG_BT_IDX_NUM; slot++) {
        if (peer_address_dirty[slot] || le_bond_dirty[slot]) return 1;
    }
    return 0;
}

/* Write what changed.  Only from an idle application task (controller.c),
 * with interrupts masked (see btstack_jl_link_key_flush()); held back in RAM
 * mode. */
void cyclone2_settings_flush(void)
{
    u8 slot;

    if (cyclone2_store_ram_mode() || !cyclone2_settings_pending()) return;
    local_irq_disable();
    if (base_mac_addr_dirty) {
        base_mac_addr_dirty = 0U;
        syscfg_write(CFG_BT_MAC_ADDR, base_mac_addr, sizeof(base_mac_addr));
    }
    for (slot = 0U; slot < CFG_BT_IDX_NUM; slot++) {
        if (peer_address_dirty[slot]) {
            peer_address_dirty[slot] = 0U;
            cyclone2_bringup_record(0x3f80u | slot);
            syscfg_write((u16)(CFG_EDR_ADDRESS_BEGIN + slot),
                         peer_address_cache[slot], 6U);
        }
        if (le_bond_dirty[slot]) {
            le_bond_dirty[slot] = 0U;
            syscfg_write((u16)(CFG_BLE_BONDING_REMOTE_INFO + slot),
                         le_bond[slot], LE_BOND_SIZE);
        }
    }
    local_irq_enable();
}
