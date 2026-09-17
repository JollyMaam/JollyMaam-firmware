#include "system/includes.h"
#include "usb_control.h"

const int vm_max_size_config = VM_MAX_SIZE_CONFIG; // from cflags

u8 get_vbat_percent(void)
{
  return 0xff;
}

u16 update_result_get(void)
{
  return 0; /* UPDATA_NON from include_lib/update/update.h */
};

u8 get_power_on_status(void)
{
    return 0;
}

const struct btif_item btif_table[] = {
    {CFG_BT_MAC_ADDR, 6 },
    {CFG_BT_FRE_OFFSET, 6 },
    {0, 0 },
};

static u8 mac_addr[6] = { 0x18, 0x28, 0x58, 0xdb, 0x11, 0x13, };
static u8 edr_name[32] = "CYCLONE2";

u8 *bt_get_mac_addr(void)
{
  //cyclone2_usb_debug("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", __func__, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  return mac_addr;
}

void bt_set_mac_addr(u8 *addr)
{
  cyclone2_usb_debug("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", __func__, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  memcpy(mac_addr, addr, 6);
}

void bt_set_local_name(char *name, u8 len)
{
  memset(edr_name, 0, sizeof(edr_name));
  memcpy(edr_name, name, len);
  edr_name[31] = 0;
}

const char *bt_get_local_name()
{
    return (const char *)edr_name;
}
