/* LE 5.0 advertising, scanning, periodic sync and PHY HCI commands.  The
 * SDK's packed offsets, allocation sizes, partial updates and status quirks
 * are retained, including unchecked caller-size copies. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG u8 log_tag_const_i_HCI_LL5, log_tag_const_e_HCI_LL5;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern struct le_parameter le_param;
extern u8 ll_periodic_advertising_terminate_sync_ir(u16 handle) __asm__("ll_periodic_advertising_terminate_sync");

#define HCI5_CODE __attribute__((section(".ble_hci_code")))

struct ext_le_parameter ext_le_param __attribute__((section(".ble_hci_bss")));

#ifndef UNITTEST
_Static_assert(sizeof(struct ext_le_parameter) == 28, "SDK ext_le_parameter size");
_Static_assert(offsetof(struct ext_le_parameter, ext_adv_list) == 0, "SDK ext adv list offset");
_Static_assert(offsetof(struct ext_le_parameter, ext_scan) == 12, "SDK ext scan offset");
_Static_assert(offsetof(struct ext_le_parameter, ext_init) == 16, "SDK ext init offset");
_Static_assert(offsetof(struct ext_le_parameter, default_prefers_phys) == 20, "SDK default PHY offset");
_Static_assert(offsetof(struct ext_le_parameter, phy_update_param) == 22, "SDK PHY update offset");
_Static_assert(sizeof(struct __ext_adv_group) == 304, "SDK adv allocation");
_Static_assert(offsetof(struct __ext_adv_group, ext_adv_param) == 8, "SDK advertising parameter offset");
_Static_assert(offsetof(struct __ext_adv_group, ext_adv_data) == 33, "SDK data offset");
_Static_assert(offsetof(struct __ext_adv_group, ext_adv_en) == 292, "SDK enable offset");
_Static_assert(offsetof(struct __ext_adv_group, periodic_info) == 296, "SDK periodic offset");
_Static_assert(sizeof(struct __ext_adv_list) == 12, "SDK advertising list size");
_Static_assert(offsetof(struct __ext_adv_list, nums_set) == 8, "SDK advertising set count offset");
_Static_assert(sizeof(struct list_head) == 8, "SDK list node size");
_Static_assert(offsetof(struct ble_ext_period_sync, list) == 14, "SDK periodic list offset");
_Static_assert(offsetof(struct ble_ext_period_sync, sync_timeout) == 30, "SDK periodic timeout offset");
_Static_assert(offsetof(struct ble_ext_period_sync, sync_handle) == 34, "SDK periodic status byte follows handle");
_Static_assert(offsetof(struct __ext_scan, ext_scan_enable) == 4, "SDK scan enable offset");
_Static_assert(offsetof(struct __ext_scan, ext_scan_param) == 10, "SDK scan param offset");
_Static_assert(sizeof(struct ble_ext_period_sync) == 37, "SDK periodic sync size");
#endif
_Static_assert(offsetof(struct __ext_init, init_phy_param) == 10, "SDK init PHY data offset");
_Static_assert(sizeof(struct ext_advertising_param) == 25, "SDK advertising parameter size");
_Static_assert(offsetof(struct ext_advertising_param, Advertising_Handle) == 0, "SDK advertising handle offset");
_Static_assert(sizeof(struct ext_advertising_data) == 259, "SDK advertising data size");
_Static_assert(offsetof(struct ext_advertising_data, cur_len) == 2, "SDK advertising data length offset");
_Static_assert(offsetof(struct ext_advertising_data, data) == 4, "SDK advertising payload offset");
_Static_assert(sizeof(struct ext_periodic_info) == 6, "SDK periodic advertising parameters size");
_Static_assert(offsetof(struct ext_periodic_info, enable) == 5, "SDK periodic enable offset");
_Static_assert(sizeof(struct __ext_init) == 10, "SDK init parameter prefix size");

static void list_add_tail(struct list_head *_new)
{
    struct list_head *prev = ext_le_param.ext_adv_list.head.prev;
    ext_le_param.ext_adv_list.head.prev = _new;
    _new->next = &ext_le_param.ext_adv_list.head;
    _new->prev = prev;
    prev->next = _new;
}

static struct ble_ext_period_sync *alloc_periodic_sync(void) __attribute__((section(".ble_hci_code")));
static struct ble_ext_period_sync *alloc_periodic_sync(void)
{
    struct __ext_scan *scan = ext_le_param.ext_scan;
    if (!scan->period_sync) {
        struct ble_ext_period_sync *allocated = __bt_malloc(37);
        ext_le_param.ext_scan->period_sync = allocated;
        if (!allocated && log_tag_const_e_HCI_LL5)
            log_print(4, NULL, "<Error>: [HCI_LL5]ext sync MEMORY_CAPACITY_EXCEEDED\r\n");
        return allocated;
    }
    return scan->period_sync;
}

static u8 get_if_phy_support(u8 phys) __attribute__((section(".ble_hci_code")));
static u8 get_if_phy_support(u8 phys)
{
    if ((phys & 2) && !(config_btctler_le_features & 256)) return 0;
    if ((phys & 4) && !(config_btctler_le_features & 2048)) return 0;
    return 1;
}

static void HCI5_CODE info(const char *s) { if (log_tag_const_i_HCI_LL5) log_print(2, NULL, s); }
static void HCI5_CODE error(const char *s) { if (log_tag_const_e_HCI_LL5) log_print(4, NULL, s); }
static void HCI5_CODE info_arg(const char *s, unsigned v)
{ if (log_tag_const_i_HCI_LL5) log_print(2, NULL, s, v); }
static void HCI5_CODE info_str(const char *s, const char *v)
{ if (log_tag_const_i_HCI_LL5) log_print(2, NULL, s, v); }
static int HCI5_CODE complete(int opcode, int status)
{ return hci_send_event_cmd_complete(opcode, "1", status); }
static int HCI5_CODE ext_data(int opcode, const u8 *data, int scan_rsp)
{
    u8 handle = data[0], operation = data[1], fragment = data[2], len = data[3];
    struct __ext_adv_group *set = ll_match_ext_adv_set_addr(handle);
    int status = 0;
    if (!set) {
        error(scan_rsp ? "<Error>: [HCI_LL5]ext rsp data UNKNOWN_ADVERTISING_IDENTIFIER\r\n" : "<Error>: [HCI_LL5]ext adv data UNKNOWN_ADVERTISING_IDENTIFIER\r\n");
        status = 66;
    } else if (!scan_rsp && operation < 3 && set->ext_adv_en.enable) {
        error("<Error>: [HCI_LL5]ext data set COMMAND_DISALLOWED\r\n"); status = 12;
    } else if (scan_rsp && operation < 3 && set->ext_adv_en.enable) {
        error("<Error>: [HCI_LL5]ext rsp data set COMMAND_DISALLOWED\r\n"); status = 12;
    } else if (!scan_rsp && operation < 3 && !len) {
        error("<Error>: [HCI_LL5]ext data set INVALID_HCI_COMMAND_PARAMETERS\r\n"); status = 18;
    } else {
        struct ext_advertising_data *d = &set->ext_adv_data;
        if (!scan_rsp && operation >= 3 && operation == 4) len = 0;
        u16 old = d->cur_len;
        if (operation == 1 || operation == 3) { d->cur_len = 0; old = 0; }
        d->Operation = operation;
        d->Fragment_Preference = fragment;
        if ((unsigned)old + len > 255) {
            error(scan_rsp ? "<Error>: [HCI_LL5]ext rsp data set MEMORY_CAPACITY_EXCEEDED\r\n" : "<Error>: [HCI_LL5]ext adv data MEMORY_CAPACITY_EXCEEDED\r\n");
            status = 7;
        } else {
            memcpy(d->data + old, data + 4, len);
            d->cur_len = old + len;
            if (!scan_rsp && set->ext_adv_en.enable) status = ll_ext_adv_data_update(handle, set);
            else if (!scan_rsp) info("[Info]: [HCI_LL5]ext data set succ\r\n");
            else info("[Info]: [HCI_LL5]ext rsp data set succ\r\n");
        }
    }
    return complete(opcode, status);
}

static int HCI5_CODE set_ext_adv_param(int opcode, const u8 *data)
{
    info("[Info]: [HCI_LL5]HCI_LE_SET_EXT_ADVERTISING_PARAMETERS\r\n");
    struct __ext_adv_group *set = ll_match_ext_adv_set_addr(data[0]);
    int status = 0;
    if (set) {
        if (set->ext_adv_en.enable) { error("<Error>: [HCI_LL5]ext adv set COMMAND_DISALLOWED\r\n"); status = 12; }
        else { info("[Info]: [HCI_LL5]ext adv set same handle succ\r\n"); memcpy(&set->ext_adv_param, data, 25); }
    } else if (ext_le_param.ext_adv_list.nums_set == 2) {
        error("<Error>: [HCI_LL5]ext adv set MEMORY_CAPACITY_EXCEEDED\r\n"); status = 7;
    } else if (!(set = __bt_malloc(304))) {
        error("<Error>: [HCI_LL5]ext adv set malloc error\r\n"); status = 7;
    } else {
        list_add_tail(&set->entry); ext_le_param.ext_adv_list.nums_set++;
        memcpy(&set->ext_adv_param, data, 25); info("[Info]: [HCI_LL5]ext adv set new succ\r\n");
    }
    return hci_send_event_cmd_complete(opcode, "11", status, le_param.transmit_power_level);
}

static int HCI5_CODE set_ext_adv_enable(int opcode, const u8 *data, int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_SET_EXT_ADVERTISE_ENABLE\r\n");
    u8 enable = data[0], count = data[1];
    const u8 *handles = data + 2, *durations = handles + count, *events = durations + (count << 1);
    int status = 0;
    if (enable && !count) { error("<Error>: [HCI_LL5]ext enable INVALID_HCI_COMMAND_PARAMETERS0\r\n"); return complete(opcode, 18); }
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = i + 1; j < count; ++j) if (handles[i] == handles[j]) {
            error("<Error>: [HCI_LL5]ext enable INVALID_HCI_COMMAND_PARAMETERS1\r\n"); return complete(opcode, 18);
        }
        if (!ll_match_ext_adv_set_addr(handles[i])) {
            error("<Error>: [HCI_LL5]ext enable UNKNOWN_ADVERTISING_IDENTIFIER\r\n"); return complete(opcode, 66);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        struct __ext_adv_group *set = ll_match_ext_adv_set_addr(handles[i]);
        if (enable) {
            set->ext_adv_en.duration = btctler_little_endian_read_16(durations, i * 2);
            set->ext_adv_en.max_ext_adv_events = events[i];
            if (set->ext_adv_en.enable) status = ll_ext_adv_alive_timer_reset(handles[i], set);
            else if (set->ext_adv_param.Advertising_Event_Properties & 16) status = ll_ext_legacy_adv_open(handles[i], set);
            else status = ll_ext_adv_open(handles[i], set);
        } else status = ll_ext_adv_close(handles[i]);
        set->ext_adv_en.enable = enable;
        info_str("[Info]: [HCI_LL5]ext adv status is %s\r\n", enable ? "<enable>" : "<disable>");
    }
    (void)size; return complete(opcode, status);
}

static int HCI5_CODE remove_adv_set(int opcode, const u8 *data)
{
    info("[Info]: [HCI_LL5]HCI_LE_REMOVE_ADVERTISING_SET\r\n");
    u8 handle=data[0];
    struct __ext_adv_group *set = ll_match_ext_adv_set_addr(handle); int status=0;
    if (!set) { error("<Error>: [HCI_LL5]remove adv set UNKNOWN_ADVERTISING_IDENTIFIER\r\n"); status=66; }
    else if (set->ext_adv_en.enable || (set->periodic_info && set->periodic_info->enable)) {
        error(set->ext_adv_en.enable ? "<Error>: [HCI_LL5]remove adv set COMMAND_DISALLOWED\r\n" : "<Error>: [HCI_LL5]remove adv set COMMAND_DISALLOWED because periodic\r\n"); status=12;
    } else { ll_remove_assigned_adv_set(set); info("[Info]: [HCI_LL5]remove adv set succ\r\n"); }
    return complete(opcode,status);
}

static int HCI5_CODE clear_adv_sets(int opcode)
{
    info("[Info]: [HCI_LL5]HCI_LE_CLEAR_ADVERTISING_SETS\r\n");
    for (struct list_head *n=ext_le_param.ext_adv_list.head.next; n != &ext_le_param.ext_adv_list.head; n=n->next) {
        struct __ext_adv_group *set=(void *)((u8 *)n - offsetof(struct __ext_adv_group,entry));
        if (set->ext_adv_en.enable || (set->periodic_info && set->periodic_info->enable)) {
            error("<Error>: [HCI_LL5]clear adv set COMMAND_DISALLOWED\r\n"); return complete(opcode,12);
        }
    }
    ll_remove_all_adv_set(); info("[Info]: [HCI_LL5]clear adv set succ\r\n"); return complete(opcode,0);
}

static int HCI5_CODE periodic_param(int opcode, const u8 *data)
{
    info("[Info]: [HCI_LL5]HCI_LE_SET_PERIODIC_ADVERTISING_PARAMETERS\r\n");
    struct __ext_adv_group *set=ll_match_ext_adv_set_addr(data[0]); int status=0;
    if (!set) { error("<Error>: [HCI_LL5]periodic param set UNKNOWN_ADVERTISING_IDENTIFIER\r\n"); status=66; }
    else if (set->ext_adv_param.Advertising_Event_Properties & 32) { error("<Error>: [HCI_LL5]periodic param set INVALID_HCI_COMMAND_PARAMETERS\r\n"); status=18; }
    else {
        if (!set->periodic_info) {
            set->periodic_info=__bt_malloc(6);
            if (!set->periodic_info) { error("<Error>: [HCI_LL5]periodic param set MEMORY_CAPACITY_EXCEEDED\r\n"); return complete(opcode,7); }
        }
        if (set->periodic_info->enable) { error("<Error>: [HCI_LL5]periodic param set COMMAND_DISALLOWED\r\n"); status=12; }
        else {
            set->periodic_info->Periodic_Advertising_Interval_Min=btctler_little_endian_read_16(data,1);
            set->periodic_info->Periodic_Advertising_Interval_Max=btctler_little_endian_read_16(data,3);
            set->periodic_info->Periodic_Advertising_Properties=data[5];
            info("[Info]: [HCI_LL5]periodic param set succ\r\n");
        }
    }
    return complete(opcode,status);
}

static int HCI5_CODE periodic_data(int opcode, const u8 *data)
{
    info("[Info]: [HCI_LL5]HCI_LE_SET_PERIODIC_ADVERTISING_DATA\r\n");
    u8 handle=data[0], op=data[1], len=data[2]; struct __ext_adv_group *set=ll_match_ext_adv_set_addr(handle); int status=0;
    if (!set) { error("<Error>: [HCI_LL5]periodic data set UNKNOWN_ADVERTISING_IDENTIFIER\r\n"); status=66; }
    else if (!set->periodic_info) { error("<Error>: [HCI_LL5]periodic data set COMMAND_DISALLOWED\r\n"); status=12; }
    else if (op<3 && set->periodic_info->enable) { error("<Error>: [HCI_LL5]periodic data set COMMAND_DISALLOWED\r\n"); status=12; }
    else {
        u16 old=set->ext_adv_data.cur_len;
        if (op==1 || op==3) { set->ext_adv_data.cur_len=0; old=0; }
        set->ext_adv_data.Operation=op;
        if ((unsigned)old+len>255) { error("<Error>: [HCI_LL5]periodic data set MEMORY_CAPACITY_EXCEEDED\r\n"); status=7; }
        else {
            memcpy(set->ext_adv_data.data+old,data+3,len); set->ext_adv_data.cur_len=old+len;
            if (set->periodic_info->enable) status=ll_ext_adv_data_update(handle,set);
            else info("[Info]: [HCI_LL5]periodic data set succ\r\n");
        }
    }
    return complete(opcode,status);
}

static int HCI5_CODE periodic_enable(int opcode, const u8 *data)
{
    info("[Info]: [HCI_LL5]HCI_LE_SET_PERIODIC_ADVERTISING_ENABLE\r\n");
    u8 enable=data[0],handle=data[1]; struct __ext_adv_group *set=ll_match_ext_adv_set_addr(handle); int status=0;
    if (!set) { error("<Error>: [HCI_LL5]periodic enable UNKNOWN_ADVERTISING_IDENTIFIER\r\n"); status=66; }
    else if (!set->periodic_info) { error("<Error>: [HCI_LL5]periodic adv not exist\r\n"); status=12; }
    else if (set->ext_adv_data.Operation<2) { error("<Error>: [HCI_LL5]periodic enable COMMAND_DISALLOWED\r\n"); status=12; }
    else if (!set->periodic_info->enable && set->ext_adv_en.enable) { error("<Error>: [HCI_LL5]periodic enable :ext adv already open\r\n"); status=12; }
    else {
        if (enable) { if (!set->periodic_info->enable) status=ll_periodic_adv_open(handle,set); }
        else ll_periodic_adv_close(handle);
        set->periodic_info->enable=enable;
        info_str("[Info]: [HCI_LL5]periodic adv status is %s\r\n",enable?"<enable>":"<disable>");
    }
    return complete(opcode,status);
}

static int HCI5_CODE ext_scan_param(int opcode,const u8 *data,int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_SET_EXTENDED_SCAN_PARAMETERS\n\r\n");
    u8 phys=data[2]; int status=0;
    if (ext_le_param.ext_scan && ext_le_param.ext_scan->ext_scan_enable.Enable) { error("<Error>: [HCI_LL5]ext scan param COMMAND_DISALLOWED\r\n"); status=12; }
    else if (!phys) { info_arg("[Info]: [HCI_LL5]phys=0x%x\r\n",phys); error("<Error>: [HCI_LL5]ext scan param INVALID_HCI_COMMAND_PARAMETERS\r\n"); status=18; }
    else {
        info_arg("[Info]: [HCI_LL5]phys=0x%x\r\n",phys);
        int phy_size=(phys&1)?5:0;
        int alloc_size=phy_size + (((config_btctler_le_features&2048) && (phys&4))?5:0);
        if (!alloc_size) info("[Info]: [HCI_LL5]ext scan param succ of none\r\n");
        else {
            int bytes=alloc_size+13; info_arg("[Info]: [HCI_LL5]alloc_size=0x%x\r\n",bytes);
            struct __ext_scan *scan=__bt_malloc(bytes);
            if (!scan) { error("<Error>: [HCI_LL5]ext scan param MEMORY_CAPACITY_EXCEEDED\r\n"); status=7; }
            else { ext_le_param.ext_scan=scan; memcpy(&scan->ext_scan_param,data,size); if (log_tag_const_i_HCI_LL5) { printf_buf(&scan->ext_scan_param,size); info("[Info]: [HCI_LL5]ext scan param succ\r\n"); } }
        }
    }
    return complete(opcode,status);
}

static int HCI5_CODE ext_scan_enable(int opcode,const u8 *data,int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_EXTENDED_SCAN_ENABLE\n\r\n");
    u8 enable=data[0],filter=data[1]; u16 duration=btctler_little_endian_read_16(data,2),period=btctler_little_endian_read_16(data,4);
    struct __ext_scan *scan=ext_le_param.ext_scan; int status=0;
    if (enable && scan) {
        if (scan->ext_scan_enable.Enable) { info("[Info]: [HCI_LL5]ext scan alreay enable\r\n"); }
        else if (filter==2 && !period) { error("<Error>: [HCI_LL5]ext scan enable (filter == 2) && (period == 0)\r\n"); status=18; }
        else if (duration && period && duration*10u >= period*1280u) { error("<Error>: [HCI_LL5]ext scan enable duration_ms >= period_ms\r\n"); status=18; }
        else { memcpy(&scan->ext_scan_enable,data,size); if (log_tag_const_i_HCI_LL5) printf_buf(&scan->ext_scan_enable,size); status=ll_ext_scan_open(); info_str("[Info]: [HCI_LL5]ext scan status is %s\r\n",enable?"<enable>":"<disable>"); }
    } else {
        if (scan) { ll_ext_scan_close(); __bt_free(ext_le_param.ext_scan); ext_le_param.ext_scan=NULL; }
        info_str("[Info]: [HCI_LL5]ext scan status is %s\r\n",enable?"<enable>":"<disable>");
    }
    return complete(opcode,status);
}

static int HCI5_CODE ext_create_connection(int opcode,const u8 *data,int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_EXTENDED_CREATE_CONNECTION\n\r\n");
    u8 phys=data[9]; int bytes=10;
    info_str("[Info]: [HCI_LL5]--func=%s\r\n","le_extended_create_connection");
    if (ext_le_param.ext_init) { error("<Error>: [HCI_LL5]ext init COMMAND_DISALLOWED\n\r\n"); return hci_send_event(15,"112",12,1,opcode); }
    info_arg("[Info]: [HCI_LL5]phys=0x%x\n\r\n",phys);
    if (!phys) { error("<Error>: [HCI_LL5]ext init INVALID_HCI_COMMAND_PARAMETERS\r\n"); return hci_send_event(15,"112",18,1,opcode); }
    for (int b=0;b<3;b++) if (phys & (1u<<b)) bytes+=16;
    info_arg("[Info]: [HCI_LL5]alloc_size=0x%x\n\r\n",bytes); info_arg("[Info]: [HCI_LL5]size=0x%x\n\r\n",size);
    struct __ext_init *init=__bt_malloc(bytes);
    if (!init) { error("<Error>: [HCI_LL5]ext init MEMORY_CAPACITY_EXCEEDED\r\n"); return hci_send_event(15,"112",7,1,opcode); }
    memcpy(init,data,size); ext_le_param.ext_init=init;
    if (log_tag_const_i_HCI_LL5) printf_buf(init,bytes);
    int status=ll_ext_init_open(); info("[Info]: [HCI_LL5]ext init succ\r\n");
    return hci_send_event(15,"112",status,1,opcode);
}

static int HCI5_CODE periodic_create_sync(int opcode,const u8 *data,int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_PERIODIC_ADVERTISING_CREATE_SYNC\n\r\n");
    if (!ext_le_param.ext_scan) { error("<Error>: [HCI_LL5]Can not creat sync before creat ext scan\r\n"); return hci_send_event(15,"112",12,1,opcode); }
    struct ble_ext_period_sync *sync=alloc_periodic_sync();
    if (!sync) return hci_send_event(15,"112",7,1,opcode);
    u8 *state=(u8 *)sync+36;
    if (*state>63) { error("<Error>: [HCI_LL5]periodic_creat_sync COMMAND_DISALLOWED\r\n"); return hci_send_event(15,"112",12,1,opcode); }
    memcpy(sync,data,size); ll_periodic_advertising_create_sync(); *state=(*state&63)|64;
    if (log_tag_const_i_HCI_LL5) { printf_buf(sync,size); info("[Info]: [HCI_LL5]periodic create sync succ\r\n"); }
    return hci_send_event(15,"112",0,1,opcode);
}

static int HCI5_CODE periodic_create_cancel(int opcode)
{
    info("[Info]: [HCI_LL5]HCI_LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL\n\r\n");
    struct __ext_scan *scan=ext_le_param.ext_scan;
    if (!scan || !scan->period_sync) { error("<Error>: [HCI_LL5]periodic_creat_sync_cancel COMMAND_DISALLOWED 1\r\n"); return complete(opcode,12); }
    u8 *state=(u8 *)scan->period_sync+36;
    if ((*state&0xc0)==0x80) { error("<Error>: [HCI_LL5]periodic_creat_sync_cancel COMMAND_DISALLOWED 0\r\n"); return complete(opcode,12); }
    ll_periodic_advertising_create_sync_cancel(); info("[Info]: [HCI_LL5]periodic_creat_sync_cancel succ\r\n");
    return complete(opcode,0);
}

static int HCI5_CODE periodic_terminate(int opcode,const u8 *data)
{
    info("[Info]: [HCI_LL5]HCI_LE_PERIODIC_ADVERTISING_TERMINATE_SYNC\n\r\n");
    struct __ext_scan *scan=ext_le_param.ext_scan;
    if (!scan || !scan->period_sync) { error("<Error>: [HCI_LL5]periodic_terminate_sync COMMAND_DISALLOWED 1\r\n"); return complete(opcode,12); }
    u16 handle=btctler_little_endian_read_16(data,0);
    u8 *state=(u8 *)scan->period_sync+36;
    if ((*state&0xc0)!=0x80) { error("<Error>: [HCI_LL5]periodic_terminate_sync COMMAND_DISALLOWED 0\r\n"); return complete(opcode,12); }
    int status=ll_periodic_advertising_terminate_sync_ir(handle)?0:66;
    if (status) error("<Error>: [HCI_LL5]periodic_terminate_sync UNKNOWN_ADVERTISING_IDENTIFIER\r\n");
    else info("[Info]: [HCI_LL5]periodic_terminate_sync succ\r\n");
    return complete(opcode,status);
}

static int HCI5_CODE periodic_add(int opcode,const u8 *data,int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST\n\r\n");
    if (!ext_le_param.ext_scan) { error("<Error>: [HCI_LL5]Can not add sync list before creat ext scan\r\n"); return complete(opcode,12); }
    struct ble_ext_period_sync *sync=alloc_periodic_sync(); if (!sync) return complete(opcode,7);
    u8 *base=(u8 *)sync+14, *state=(u8 *)sync+36;
    for (int i=0;i<2;i++) if (!memcmp(data,base+i*8,size)) { error("<Error>: [HCI_LL5]add_periodic_advertiser_list INVALID_HCI_COMMAND_PARAMETERS\r\n"); return complete(opcode,18); }
    if ((*state&0xc0)==0x40) { error("<Error>: [HCI_LL5]add_periodic_advertiser_list COMMAND_DISALLOWED\r\n"); return complete(opcode,12); }
    if ((*state&62)>1) { error("<Error>: [HCI_LL5]add_periodic_advertiser_list MEMORY_CAPACITY_EXCEEDED\r\n"); return complete(opcode,7); }
    for (int i=0;i<2;i++) if (btctler_memcmp_zero(base+i*8,size)==0) { memcpy(base+i*8,data,size); break; }
    *state=((*state+1)&63)|(*state&0xc0); info("[Info]: [HCI_LL5]add_periodic_advertiser_list succ\r\n"); return complete(opcode,0);
}

static int HCI5_CODE periodic_remove(int opcode,const u8 *data,int size)
{
    info("[Info]: [HCI_LL5]HCI_LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST\n\r\n");
    struct __ext_scan *scan=ext_le_param.ext_scan;
    if (!scan || !scan->period_sync) { error("<Error>: [HCI_LL5]remove_periodic_advertiser_list COMMAND_DISALLOWED 1\r\n"); return complete(opcode,12); }
    struct ble_ext_period_sync *sync=scan->period_sync; u8 *state=(u8 *)sync+36, *base=(u8 *)sync+14;
    if ((*state&0xc0)==0x40) { error("<Error>: [HCI_LL5]remove_periodic_advertiser_list COMMAND_DISALLOWED 0\r\n"); return complete(opcode,12); }
    for (int i=0;i<2;i++) if (!memcmp(data,base+i*8,size)) {
        memset(base+i*8,0,size); *state=((*state+63)&63)|(*state&0xc0); info("[Info]: [HCI_LL5]remove_periodic_advertiser_list succ\r\n"); return complete(opcode,0);
    }
    error("<Error>: [HCI_LL5]remove_periodic_advertiser_list UNKNOWN_ADVERTISING_IDENTIFIER\r\n"); return complete(opcode,66);
}

static int HCI5_CODE periodic_clear(int opcode)
{
    info("[Info]: [HCI_LL5]HCI_LE_CLEAR_PERIODIC_ADVERTISER_LIST\n\r\n");
    struct __ext_scan *scan=ext_le_param.ext_scan;
    if (!scan || !scan->period_sync) { error("<Error>: [HCI_LL5]clear_periodic_advertiser_list COMMAND_DISALLOWED 1\r\n"); return complete(opcode,12); }
    u8 *state=(u8 *)scan->period_sync+36; unsigned high=*state&0xc0;
    if (high==0x40) { error("<Error>: [HCI_LL5]clear_periodic_advertiser_list COMMAND_DISALLOWED 0\r\n"); return complete(opcode,12); }
    memset((u8 *)scan->period_sync+14,0,16); *state=high;
    info("[Info]: [HCI_LL5]clear_periodic_advertiser_list succ\r\n"); return complete(opcode,0);
}

static int HCI5_CODE phy_common(int opcode,const u8 *data)
{
    switch ((u16)opcode & 0x3ff) {
    case 48: {
        info("[Info]: [HCI_LL5]HCI_LE_READ_PHY\n\r\n");
        u16 handle=btctler_little_endian_read_16(data,0); u8 tx=0,rx=0;
        int status=ll_read_phy(handle,&tx,&rx);
        return hci_send_event_cmd_complete(opcode,"1211",status,handle,(int)tx+1,(int)rx+1);
    }
    case 49: {
        info("[Info]: [HCI_LL5]HCI_LE_SET_DEFAULT_PHY\n\r\n");
        u8 all=data[0],tx=data[1],rx=data[2];
        if (!(all&1)) {
            if (!get_if_phy_support(tx)) { info_arg("[Info]: [HCI_LL5]PHY UNSUPPORT=%d\n\r\n",tx); hci_send_event_cmd_complete(opcode,"1",31); return 1; }
            ext_le_param.default_prefers_phys.tx_phys=tx;
        }
        u8 result_rx=ext_le_param.default_prefers_phys.rx_phys;
        if (!(all&2)) {
            if (!get_if_phy_support(rx)) { info_arg("[Info]: [HCI_LL5]PHY UNSUPPORT=%d\n\r\n",rx); hci_send_event_cmd_complete(opcode,"1",31); return 1; }
            ext_le_param.default_prefers_phys.rx_phys=rx; result_rx=rx;
        }
        ext_le_param.phy_update_param.prefers_tx_phys=ext_le_param.default_prefers_phys.tx_phys;
        ext_le_param.phy_update_param.prefers_rx_phys=result_rx;
        return hci_send_event_cmd_complete(opcode,"1",0);
    }
    case 50: {
        info("[Info]: [HCI_LL5]HCI_LE_SET_PHY\n\r\n");
        u8 all=data[2],tx=data[3],rx=data[4];
        u8 tx_supported=get_if_phy_support(tx), rx_supported=get_if_phy_support(rx);
        if (((unsigned)(tx|rx)>7) | !tx_supported | !rx_supported) return hci_send_event(15,"112",17,1,opcode);
        if (!all && tx!=rx) return hci_send_event(15,"112",17,1,opcode);
        u16 handle=btctler_little_endian_read_16(data,0),options=btctler_little_endian_read_16(data,5);
        ext_le_param.phy_update_param.prefers_tx_phys=ext_le_param.default_prefers_phys.tx_phys;
        ext_le_param.phy_update_param.prefers_rx_phys=ext_le_param.default_prefers_phys.rx_phys;
        ext_le_param.phy_update_param.phy_options=options;
        if (!(all&1)) ext_le_param.phy_update_param.prefers_tx_phys=tx;
        if (!(all&2)) ext_le_param.phy_update_param.prefers_rx_phys=rx;
        int status=ll_set_phy(handle); return hci_send_event(15,"112",status,1,opcode);
    }
    default: return 0;
    }
}

int le_controller_handler_for_ble5(u16 opcode,const u8 *data,u32 size) __attribute__((section(".ble_hci_code")));
int le_controller_handler_for_ble5(u16 opcode,const u8 *data,u32 size)
{
    int match=0; u16 ocf=opcode&0x3ff; u64 features=config_btctler_le_features;
    if (config_btctler_le_roles&2) {
        if (features&4096) {
            switch (ocf) {
            case 54: (void)set_ext_adv_param(opcode,data); match=1; break;
            case 55: info("[Info]: [HCI_LL5]HCI_LE_SET_EXT_ADVERTISING_DATA\r\n"); (void)ext_data(opcode,data,0); match=1; break;
            case 56: info("[Info]: [HCI_LL5]HCI_LE_SET_EXT_SCAN_RESPONSE_DATA\r\n"); (void)ext_data(opcode,data,1); match=1; break;
            case 57: (void)set_ext_adv_enable(opcode,data,size); match=1; break;
            case 58: info("[Info]: [HCI_LL5]HCI_LE_READ_MAXIMUM_ADVERTISING_DATA_LENGTH\r\n"); hci_send_event_cmd_complete(opcode,"12",0,255); match=1; break;
            case 59: info("[Info]: [HCI_LL5]HCI_LE_READ_NUM_OF_SUPPORTED_ADV_SETS\r\n"); hci_send_event_cmd_complete(opcode,"11",0,2); match=1; break;
            case 60: (void)remove_adv_set(opcode,data); match=1; break;
            case 61: (void)clear_adv_sets(opcode); match=1; break;
            default: break;
            }
            if (features&8192) {
                int prior=match;
                switch (ocf) {
                case 62: (void)periodic_param(opcode,data); match=prior^1; break;
                case 63: (void)periodic_data(opcode,data); match=prior^1; break;
                case 64: (void)periodic_enable(opcode,data); match=prior^1; break;
                default: break;
                }
            }
        }
    }
    if (config_btctler_le_roles&1) {
        if (features&4096) {
            int master_match=0;
            switch (ocf) {
            case 65: (void)ext_scan_param(opcode,data,size); master_match=1; break;
            case 66: (void)ext_scan_enable(opcode,data,size); master_match=1; break;
            case 67: (void)ext_create_connection(opcode,data,size); master_match=1; break;
            default: break;
            }
            if (features&8192) {
                int prior=master_match;
                switch (ocf) {
                case 68: (void)periodic_create_sync(opcode,data,size); master_match=prior^1; break;
                case 69: (void)periodic_create_cancel(opcode); master_match=prior^1; break;
                case 70: (void)periodic_terminate(opcode,data); master_match=prior^1; break;
                case 71: (void)periodic_add(opcode,data,size); master_match=prior^1; break;
                case 72: (void)periodic_remove(opcode,data,size); master_match=prior^1; break;
                case 73: (void)periodic_clear(opcode); master_match=prior^1; break;
                case 74: info("[Info]: [HCI_LL5]HCI_LE_READ_PERIODIC_ADVERTISER_LIST_SIZE\n\r\n"); hci_send_event_cmd_complete(opcode,"11",0,2); master_match=prior^1; break;
                default: break;
                }
            }
            match |= master_match;
        }
    }
    if (features&2304) {
        (void)phy_common(opcode,data);
        if (ocf==48 || ocf==49 || ocf==50) match |= 1;
    }
    info_arg("[Info]: [HCI_LL5]ble5 match=%d\n\r\n",match);
    return match;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

int config_btctler_le_roles;
u64 config_btctler_le_features;
u8 log_tag_const_i_HCI_LL5, log_tag_const_e_HCI_LL5;
struct le_parameter le_param;
static struct __ext_adv_group *sets[4]; static int nsets, alloc_size, fail_alloc;
static int event_calls, event_opcode, event_status, event_format, event_a, event_b, event_c;
static int hci_event_calls, ll_status, opens, closes, alive, data_updates, periodic_calls, init_calls;
static int phy_calls, read_tx, read_rx, last_phy_handle, printf_calls, log_calls;
static char last_log[192];
static char trace[256], log_text[24][192]; static int trace_len, log_text_count;
static int open_results[4], open_result_count, open_result_at;
static u8 *mutate_on_enable_log;
static void trace_add(char c) { if (trace_len+1<(int)sizeof(trace)) { trace[trace_len++]=c; trace[trace_len]=0; } }

void log_print(int level,const char *tag,const char *format,...)
{ (void)tag; va_list ap; va_start(ap,format); vsnprintf(last_log,sizeof(last_log),format,ap); va_end(ap); CHECK(level==2 || level==4); log_calls++; if(log_text_count<24) snprintf(log_text[log_text_count++],192,"%s",last_log); trace_add('L'); if(mutate_on_enable_log && !strcmp(last_log,"[Info]: [HCI_LL5]HCI_LE_SET_EXT_ADVERTISE_ENABLE\r\n")) { mutate_on_enable_log[0]=0; mutate_on_enable_log=NULL; } }
void printf_buf(const void *p,int n) { CHECK(p!=NULL); CHECK(n>=0); printf_calls++; trace_add('P'); }
void *__bt_malloc(int size) { alloc_size=size; if(fail_alloc) return NULL; return calloc(1,(size_t)size+64); }
void __bt_free(void *p) { free(p); }
u16 btctler_little_endian_read_16(const u8 *p,int off) { return p[off] | ((int)p[off+1]<<8); }
u8 btctler_memcmp_zero(const void *p,u32 n) { const u8 *b=p; for(u32 i=0;i<n;i++) if(b[i]) return 1; return 0; }
void *ll_match_ext_adv_set_addr(u8 handle)
{ for(struct list_head *n=ext_le_param.ext_adv_list.head.next;n && n!=&ext_le_param.ext_adv_list.head;n=n->next) { struct __ext_adv_group *s=(void *)((u8 *)n - offsetof(struct __ext_adv_group,entry)); if(s->ext_adv_param.Advertising_Handle==handle) return s; } return NULL; }
int hci_send_event_cmd_complete(int opcode,const char *format,...)
{
    va_list ap; va_start(ap,format); event_calls++; event_opcode=opcode; event_format=0; event_a=event_b=event_c=0;
    if(!strcmp(format,"1")) { event_format=1; event_status=va_arg(ap,int); }
    else if(!strcmp(format,"11")) { event_format=11; event_status=va_arg(ap,int); event_a=va_arg(ap,int); }
    else if(!strcmp(format,"12")) { event_format=12; event_status=va_arg(ap,int); event_a=va_arg(ap,int); }
    else if(!strcmp(format,"1211")) { event_format=1211; event_status=va_arg(ap,int); event_a=va_arg(ap,int); event_b=va_arg(ap,int); event_c=va_arg(ap,int); }
    else CHECK(0);
    va_end(ap); trace_add('E'); return 91;
}
int hci_send_event(int opcode,const char *format,...)
{ va_list ap; va_start(ap,format); hci_event_calls++; event_opcode=opcode; event_status=va_arg(ap,int); event_a=va_arg(ap,int); event_b=va_arg(ap,int); va_end(ap); CHECK(!strcmp(format,"112")); trace_add('E'); return 73; }
int ll_ext_adv_data_update(u8 h,struct __ext_adv_group *s) { (void)h;(void)s; data_updates++; return ll_status; }
int ll_ext_adv_alive_timer_reset(u8 h,struct __ext_adv_group *s) { (void)h;(void)s; alive++; return ll_status; }
int ll_ext_legacy_adv_open(u8 h,struct __ext_adv_group *s) { (void)h;(void)s; opens++; return open_result_at<open_result_count?open_results[open_result_at++]:ll_status; }
int ll_ext_adv_open(u8 h,struct __ext_adv_group *s) { (void)h;(void)s; opens++; return open_result_at<open_result_count?open_results[open_result_at++]:ll_status; }
int ll_ext_adv_close(u8 h) { (void)h; closes++; return ll_status; }
void ll_remove_assigned_adv_set(struct __ext_adv_group *s)
{ struct list_head *e=&s->entry; e->prev->next=e->next; e->next->prev=e->prev; for(int i=0;i<nsets;i++) if(sets[i]==s) sets[i]=NULL; ext_le_param.ext_adv_list.nums_set--; free(s); }
void ll_remove_all_adv_set(void)
{ struct list_head *e=ext_le_param.ext_adv_list.head.next; while(e!=&ext_le_param.ext_adv_list.head) { struct list_head *next=e->next; struct __ext_adv_group *s=(void *)((u8 *)e - offsetof(struct __ext_adv_group,entry)); free(s); e=next; } ext_le_param.ext_adv_list.head.next=ext_le_param.ext_adv_list.head.prev=&ext_le_param.ext_adv_list.head; memset(sets,0,sizeof(sets)); nsets=0; ext_le_param.ext_adv_list.nums_set=0; }
int ll_ext_scan_open(void) { opens++; trace_add('O'); return ll_status; }
void ll_ext_scan_close(void) { closes++; }
int ll_ext_init_open(void) { init_calls++; trace_add('O'); return ll_status; }
void ll_periodic_advertising_create_sync(void) { periodic_calls++; }
void ll_periodic_advertising_create_sync_cancel(void) { periodic_calls++; }
u8 ll_periodic_advertising_terminate_sync_test(u16 h) __asm__("ll_periodic_advertising_terminate_sync");
u8 ll_periodic_advertising_terminate_sync_test(u16 h) { last_phy_handle=h; periodic_calls++; return ll_status!=0; }
int ll_periodic_adv_open(u8 h,struct __ext_adv_group *s) { (void)h;(void)s; opens++; return ll_status; }
void ll_periodic_adv_close(u8 h) { (void)h; closes++; }
u8 ll_read_phy(u16 h,u8 *tx,u8 *rx) { last_phy_handle=h; *tx=read_tx; *rx=read_rx; return ll_status; }
u8 ll_set_phy(u16 h) { last_phy_handle=h; phy_calls++; return ll_status; }

static void reset_test(void)
{
    if(ext_le_param.ext_adv_list.head.next && ext_le_param.ext_adv_list.head.next!=&ext_le_param.ext_adv_list.head) ll_remove_all_adv_set();
    if(ext_le_param.ext_scan) { free(ext_le_param.ext_scan->period_sync); free(ext_le_param.ext_scan); }
    free(ext_le_param.ext_init); memset(&ext_le_param,0,sizeof(ext_le_param));
    ext_le_param.ext_adv_list.head.next=ext_le_param.ext_adv_list.head.prev=&ext_le_param.ext_adv_list.head;
    memset(&le_param,0,sizeof(le_param)); le_param.transmit_power_level=0xfe;
    config_btctler_le_roles=0; config_btctler_le_features=0x3001; log_tag_const_i_HCI_LL5=log_tag_const_e_HCI_LL5=0;
    nsets=0; memset(sets,0,sizeof(sets)); alloc_size=fail_alloc=event_calls=hci_event_calls=opens=closes=alive=data_updates=periodic_calls=init_calls=phy_calls=printf_calls=log_calls=0; ll_status=0;
    trace_len=log_text_count=open_result_count=open_result_at=0; trace[0]=0; memset(log_text,0,sizeof(log_text)); mutate_on_enable_log=NULL;
}
static struct __ext_adv_group *add_set(u8 handle,u8 props)
{
    u8 p[25]={0}; p[0]=handle;p[1]=props; config_btctler_le_roles=2; config_btctler_le_features=4096;
    CHECK(le_controller_handler_for_ble5(54,p,sizeof(p))==1); CHECK(event_status==0 && event_format==11 && event_a==0xfe && alloc_size==304);
    struct __ext_adv_group *s=ll_match_ext_adv_set_addr(handle); CHECK(s); if(nsets<4) sets[nsets++]=s; return s;
}
int main(void)
{
    reset_test(); u8 empty[20]={0};
    CHECK(le_controller_handler_for_ble5(54,empty,25)==0 && event_calls==0);
    config_btctler_le_roles=1; config_btctler_le_features=8192;
    CHECK(le_controller_handler_for_ble5(68,empty,14)==0 && hci_event_calls==0);
    config_btctler_le_roles=2; config_btctler_le_features=4096;
    CHECK(le_controller_handler_for_ble5(58,empty,0)==1 && event_format==12 && event_status==0 && event_a==255);
    struct __ext_adv_group *a=add_set(1,0), *b=add_set(2,0);
    u8 d[260]={1,0,9,10,1,2,3,4,5,6,7,8,9,10};
    CHECK(le_controller_handler_for_ble5(55,d,sizeof(d))==1 && event_status==0);
    CHECK(a->ext_adv_data.Operation==0 && a->ext_adv_data.Fragment_Preference==9 && a->ext_adv_data.cur_len==10 && a->ext_adv_data.data[9]==10);
    d[1]=0; d[2]=0x44; d[3]=246; CHECK(le_controller_handler_for_ble5(55,d,sizeof(d))==1 && event_status==7);
    CHECK(a->ext_adv_data.Operation==0 && a->ext_adv_data.Fragment_Preference==0x44 && a->ext_adv_data.cur_len==10);
    d[1]=1;d[3]=255;CHECK(le_controller_handler_for_ble5(55,d,sizeof(d))==1 && event_status==0 && a->ext_adv_data.cur_len==255);
    u8 rsp[8]={1,3,3,2,0xaa,0xbb}; CHECK(le_controller_handler_for_ble5(56,rsp,sizeof(rsp))==1 && event_status==0);
    CHECK(a->ext_adv_data.cur_len==2 && a->ext_adv_data.data[0]==0xaa && a->ext_adv_data.data[1]==0xbb);
    u8 enable[10]={1,2,1,2,0x34,0x12,0x78,0x56,7,8}; ll_status=3;
    u8 duplicate_enable[10]={1,2,1,1,0,0,0,0,0,0};
    CHECK(le_controller_handler_for_ble5(57,duplicate_enable,sizeof(duplicate_enable))==1 && event_status==18 && opens==0);
    duplicate_enable[3]=3; CHECK(le_controller_handler_for_ble5(57,duplicate_enable,sizeof(duplicate_enable))==1 && event_status==66 && opens==0);
    open_result_count=2; open_results[0]=4; open_results[1]=0;
    CHECK(le_controller_handler_for_ble5(57,enable,sizeof(enable))==1 && event_status==0 && opens==2);
    CHECK(a->ext_adv_en.duration==0x1234 && a->ext_adv_en.max_ext_adv_events==7 && b->ext_adv_en.duration==0x5678 && b->ext_adv_en.max_ext_adv_events==8);
    ll_status=9; CHECK(le_controller_handler_for_ble5(57,enable,sizeof(enable))==1 && event_status==9 && alive==2);
    u8 running_data[5]={1,3,0,0xaa}; CHECK(le_controller_handler_for_ble5(55,running_data,sizeof(running_data))==1 && event_status==9 && data_updates==1);
    ll_status=0; enable[0]=0; CHECK(le_controller_handler_for_ble5(57,enable,sizeof(enable))==1 && event_status==0 && closes==2);
    config_btctler_le_features=4096|8192;
    u8 per[6]={1,0,0,2,0,0}; a->ext_adv_param.Advertising_Event_Properties=32;
    CHECK(le_controller_handler_for_ble5(62,per,6)==1 && event_status==18 && !a->periodic_info);
    a->ext_adv_param.Advertising_Event_Properties=0; CHECK(le_controller_handler_for_ble5(62,per,6)==1 && event_status==0 && alloc_size==6 && a->periodic_info);

    /* Master role and scan feature gates are independent of the slave role. */
    reset_test(); config_btctler_le_roles=2; config_btctler_le_features=4096;
    u8 scanparam[18]={0,0,1}; CHECK(le_controller_handler_for_ble5(65,scanparam,3)==0);
    config_btctler_le_roles=1; fail_alloc=0;
    CHECK(le_controller_handler_for_ble5(65,scanparam,3)==1 && event_status==0 && alloc_size==18 && ext_le_param.ext_scan);
    u8 scanen[6]={1,1,0,0,0,0}; ll_status=7;
    CHECK(le_controller_handler_for_ble5(66,scanen,6)==1 && event_status==7 && opens==1 && ext_le_param.ext_scan->ext_scan_enable.Enable==1);
    scanen[0]=0; CHECK(le_controller_handler_for_ble5(66,scanen,6)==1 && event_status==0 && closes==1 && !ext_le_param.ext_scan);

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=4096|8192;
    u8 scanparam2[3]={0,0,1}; CHECK(le_controller_handler_for_ble5(65,scanparam2,3)==1 && ext_le_param.ext_scan);
    u8 add[8]={4,1,2,3,4,5,6,7};
    CHECK(le_controller_handler_for_ble5(71,add,8)==1 && event_status==0 && alloc_size==37);
    CHECK(((u8 *)ext_le_param.ext_scan->period_sync)[36]==1);
    CHECK(le_controller_handler_for_ble5(71,add,8)==1 && event_status==18 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==1);
    add[0]=5; CHECK(le_controller_handler_for_ble5(71,add,8)==1 && event_status==0 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==2);
    add[0]=4; CHECK(le_controller_handler_for_ble5(71,add,8)==1 && event_status==18 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==2);
    add[0]=6; CHECK(le_controller_handler_for_ble5(71,add,8)==1 && event_status==7 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==2);
    u8 key[8]={9,1,2,3,4,5,6,7}; CHECK(le_controller_handler_for_ble5(72,key,8)==1 && event_status==66); /* unknown key */
    add[0]=4; CHECK(le_controller_handler_for_ble5(72,add,8)==1 && event_status==0 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==1);
    u8 zero[8]={0};
    CHECK(le_controller_handler_for_ble5(73,zero,8)==1 && event_status==0 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==0);
    CHECK(le_controller_handler_for_ble5(72,zero,8)==1 && event_status==0 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&63)==63);
    ((u8 *)ext_le_param.ext_scan->period_sync)[36]=64;
    CHECK(le_controller_handler_for_ble5(71,add,8)==1 && event_status==12);
    CHECK(le_controller_handler_for_ble5(73,zero,8)==1 && event_status==12);

    /* Every remaining dispatcher family plus periodic data boundaries. */
    reset_test(); config_btctler_le_roles=3; config_btctler_le_features=4096|8192;
    a=add_set(3,0); config_btctler_le_roles=3; config_btctler_le_features=4096|8192;
    u8 pparam[6]={3,1,0,2,0,0};
    CHECK(le_controller_handler_for_ble5(62,pparam,6)==1 && event_status==0);
    u8 pdata[258]={3,3,255}; for(int i=3;i<258;i++) pdata[i]=(u8)i;
    CHECK(le_controller_handler_for_ble5(63,pdata,sizeof(pdata))==1 && event_status==0 && a->ext_adv_data.cur_len==255 && a->ext_adv_data.Operation==3);
    u8 over[5]={3,0,1,0xaa}; CHECK(le_controller_handler_for_ble5(63,over,sizeof(over))==1 && event_status==7 && a->ext_adv_data.Operation==0 && a->ext_adv_data.cur_len==255);
    u8 pdata_reset[5]={3,3,2,0xaa,0xbb}; CHECK(le_controller_handler_for_ble5(63,pdata_reset,sizeof(pdata_reset))==1 && event_status==0 && a->ext_adv_data.cur_len==2);
    u8 pen[2]={1,3}; ll_status=6;
    CHECK(le_controller_handler_for_ble5(64,pen,2)==1 && event_status==6 && a->periodic_info->enable==1 && opens==1);
    u8 pdata_running[5]={3,3,1,0xcc}; CHECK(le_controller_handler_for_ble5(63,pdata_running,sizeof(pdata_running))==1 && event_status==6 && data_updates==1);
    ll_status=0; pen[0]=0; CHECK(le_controller_handler_for_ble5(64,pen,2)==1 && event_status==0 && !a->periodic_info->enable && closes==1);
    CHECK(le_controller_handler_for_ble5(60,(u8[]){3},1)==1 && event_status==0 && !ll_match_ext_adv_set_addr(3));
    add_set(4,0); config_btctler_le_roles=3; config_btctler_le_features=4096|8192;
    CHECK(le_controller_handler_for_ble5(61,empty,0)==1 && event_status==0 && !ll_match_ext_adv_set_addr(4));
    CHECK(le_controller_handler_for_ble5(59,empty,0)==1 && event_format==11 && event_status==0 && event_a==2);
    CHECK(le_controller_handler_for_ble5(74,empty,0)==1 && event_format==11 && event_status==0 && event_a==2);

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=4096|8192;
    u8 scp[3]={0,0,1}; CHECK(le_controller_handler_for_ble5(65,scp,3)==1);
    u8 create[14]={0}; create[0]=0; create[9]=1;
    CHECK(le_controller_handler_for_ble5(68,create,14)==1 && event_status==0 && periodic_calls==1 && (((u8 *)ext_le_param.ext_scan->period_sync)[36]&0xc0)==0x40);
    CHECK(le_controller_handler_for_ble5(68,create,14)==1 && event_status==12 && periodic_calls==1);
    CHECK(le_controller_handler_for_ble5(69,empty,0)==1 && event_status==0 && periodic_calls==2);
    ((u8 *)ext_le_param.ext_scan->period_sync)[36]=128;
    CHECK(le_controller_handler_for_ble5(69,empty,0)==1 && event_status==12 && periodic_calls==2);
    u8 term[2]={0x34,0x12}; ll_status=1;
    CHECK(le_controller_handler_for_ble5(70,term,2)==1 && event_status==0 && last_phy_handle==0x1234 && periodic_calls==3);
    ll_status=0; CHECK(le_controller_handler_for_ble5(70,term,2)==1 && event_status==66);
    u8 initp[26]={0}; initp[9]=1; ll_status=4;
    CHECK(le_controller_handler_for_ble5(67,initp,sizeof(initp))==1 && event_status==4 && init_calls==1 && alloc_size==26 && ext_le_param.ext_init);
    CHECK(le_controller_handler_for_ble5(67,initp,sizeof(initp))==1 && event_status==12 && init_calls==1);

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=256;
    u8 default_phy[3]={0,2,4};
    CHECK(le_controller_handler_for_ble5(49,default_phy,3)==1 && event_status==31 && ext_le_param.default_prefers_phys.tx_phys==2 && ext_le_param.default_prefers_phys.rx_phys==0);
    config_btctler_le_features=256|2048;
    CHECK(le_controller_handler_for_ble5(49,default_phy,3)==1 && event_status==0 && ext_le_param.default_prefers_phys.tx_phys==2 && ext_le_param.default_prefers_phys.rx_phys==4);

    /* Allocation, operation-4, and log/call ordering edges. */
    reset_test(); config_btctler_le_roles=2; config_btctler_le_features=4096; fail_alloc=1;
    u8 param[25]={0}; param[0]=8;
    CHECK(le_controller_handler_for_ble5(54,param,sizeof(param))==1 && event_status==7 && alloc_size==304 && ext_le_param.ext_adv_list.nums_set==0);
    fail_alloc=0; a=add_set(8,0); add_set(9,0);
    param[0]=10; CHECK(le_controller_handler_for_ble5(54,param,sizeof(param))==1 && event_status==7 && ext_le_param.ext_adv_list.nums_set==2);
    a->ext_adv_data.cur_len=3; a->ext_adv_data.data[0]=1;
    u8 op4[12]={8,4,7,8,0xde,0xad,0xbe,0xef};
    CHECK(le_controller_handler_for_ble5(55,op4,sizeof(op4))==1 && event_status==0 && a->ext_adv_data.Operation==4 && a->ext_adv_data.cur_len==3);
    a->ext_adv_en.enable=1; u8 log_mutation[6]={1,1,8,0,0,0}; int old_closes=closes;
    log_tag_const_i_HCI_LL5=1; mutate_on_enable_log=log_mutation;
    CHECK(le_controller_handler_for_ble5(57,log_mutation,sizeof(log_mutation))==1 && event_status==0 && !a->ext_adv_en.enable && closes==old_closes+1);

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=4096; log_tag_const_i_HCI_LL5=1;
    u8 initlog[26]={0}; initlog[9]=1;
    CHECK(le_controller_handler_for_ble5(67,initlog,sizeof(initlog))==1 && event_status==0);
    CHECK(!strcmp(trace,"LLLLLPOLEL") && log_text_count==7);
    CHECK(!strcmp(log_text[0],"[Info]: [HCI_LL5]HCI_LE_EXTENDED_CREATE_CONNECTION\n\r\n"));
    CHECK(!strcmp(log_text[1],"[Info]: [HCI_LL5]--func=le_extended_create_connection\r\n"));
    CHECK(!strcmp(log_text[5],"[Info]: [HCI_LL5]ext init succ\r\n"));

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=4096; log_tag_const_i_HCI_LL5=1; log_tag_const_e_HCI_LL5=1;
    u8 sparam[3]={0,0,1}; CHECK(le_controller_handler_for_ble5(65,sparam,3)==1);
    u8 sen[6]={1,2,0,0,0,0}; CHECK(le_controller_handler_for_ble5(66,sen,6)==1 && event_status==18);
    CHECK(log_text_count>=4 && !strcmp(log_text[log_text_count-3],"[Info]: [HCI_LL5]HCI_LE_EXTENDED_SCAN_ENABLE\n\r\n") && !strcmp(log_text[log_text_count-2],"<Error>: [HCI_LL5]ext scan enable (filter == 2) && (period == 0)\r\n"));
    ext_le_param.ext_scan->ext_scan_enable.Enable=1; log_text_count=0; trace_len=0; trace[0]=0;
    CHECK(le_controller_handler_for_ble5(65,sparam,3)==1 && event_status==12 && log_text_count==3);
    CHECK(!strcmp(log_text[0],"[Info]: [HCI_LL5]HCI_LE_SET_EXTENDED_SCAN_PARAMETERS\n\r\n") && !strcmp(log_text[1],"<Error>: [HCI_LL5]ext scan param COMMAND_DISALLOWED\r\n"));

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=4096;
    u8 coded[3]={0,0,0}; CHECK(le_controller_handler_for_ble5(65,coded,3)==1 && event_status==18 && alloc_size==0);
    coded[2]=4; CHECK(le_controller_handler_for_ble5(65,coded,3)==1 && event_status==0 && alloc_size==0);
    config_btctler_le_features|=2048; CHECK(le_controller_handler_for_ble5(65,coded,3)==1 && event_status==0 && alloc_size==18);
    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=4096; fail_alloc=1; coded[2]=1;
    CHECK(le_controller_handler_for_ble5(65,coded,3)==1 && event_status==7 && alloc_size==18 && !ext_le_param.ext_scan);

    reset_test(); config_btctler_le_roles=1; config_btctler_le_features=2304;
    u8 phy[8]={0}; phy[0]=0x34;phy[1]=0x12; read_tx=255;read_rx=2; ll_status=5;
    CHECK(le_controller_handler_for_ble5(48,phy,8)==1 && event_format==1211 && event_status==5 && event_a==0x1234 && event_b==256 && event_c==3);
    phy[0]=0;phy[1]=1;phy[2]=1;phy[3]=4;phy[4]=4; phy[5]=0x78;phy[6]=0x56;ll_status=4;
    CHECK(le_controller_handler_for_ble5(50,phy,8)==1 && event_status==4 && last_phy_handle==0x100 && phy_calls==1 && ext_le_param.phy_update_param.phy_options==0x5678);
    config_btctler_le_features=256; phy[3]=4;phy[4]=4;
    CHECK(le_controller_handler_for_ble5(50,phy,8)==1 && hci_event_calls>0 && event_status==17 && phy_calls==1);
    CHECK(log_calls==0);
    return TEST_DONE("btctrler_hci_ll_5_cmds");
}
#endif
