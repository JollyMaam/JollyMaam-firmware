/*
 * Jieli SDK side of the BTstack port.
 *
 * The application modules (profile_switch.c, controller.c, ...) are written
 * against the vendor host library's little API - btstack_init(),
 * btstack_edr_start_before_init(), user_send_cmd_prepare(), edr_hid_data_send()
 * and the SYS_BT_EVENT notifications.  This file keeps that API and implements
 * it on top of the upstream BTstack host in btstack_jl_core.c, so the vendor
 * btstack.a is not linked any more.
 *
 * It is the only file that includes both the Jieli SDK headers and the port
 * interface; btstack_jl_core.c stays free of SDK types.
 */

#include <system/includes.h>
#include <system/task.h>
#include <system/timer.h>
#include <btstack/avctp_user.h>

#include "app_config.h"
#include "build_config.h"
#include "usb.h"

#include "app_comm_bt.h"
#include "hci_dump_console.h"
#include "main.h"
#include "btstack_jl.h"
#include "btstack_jl_hid_ext.h"



#ifndef CFG_REMOTE_DB_00
#define CFG_REMOTE_DB_00 62
#endif

/* One entry per bonded peer: tag, address, link key, key type.
 *
 * The syscfg items we use (CFG_REMOTE_DB_00..) are the ones the vendor host
 * library used for its own 38-byte records, so a record only counts as ours
 * when it carries the tag; foreign records (from a firmware built on the
 * vendor stack) are ignored instead of being read back as a key. */
#define BTSTACK_JL_LINK_KEY_ENTRIES BTSTACK_JL_LINK_KEY_SLOTS
/* Tag byte: which slot a record belongs to.  Each slot has its own local
 * address, so a host paired on two slots holds two keys; a record keyed only
 * by the host's address let the second pairing overwrite the first, and the
 * first slot's reconnect then failed with reason 5.  Records written before
 * this carry the legacy tag and match any slot. */
#define BTSTACK_JL_LINK_KEY_TAG_LEGACY 0xC2
#define BTSTACK_JL_LINK_KEY_TAG_SLOT   0xC8  /* | owner slot 0..3 */
#define BTSTACK_JL_LINK_KEY_ANY_SLOT   0xFFU

static int link_key_tag_valid(u8 tag)
{
    return tag == BTSTACK_JL_LINK_KEY_TAG_LEGACY ||
           (tag & 0xFCU) == BTSTACK_JL_LINK_KEY_TAG_SLOT;
}

static u8 link_key_tag_for(u8 owner)
{
    return owner == BTSTACK_JL_LINK_KEY_ANY_SLOT ?
           BTSTACK_JL_LINK_KEY_TAG_LEGACY :
           (u8)(BTSTACK_JL_LINK_KEY_TAG_SLOT | (owner & 0x03U));
}

/* The slot whose session is running: its records, and legacy ones. */
static u8 link_key_owner(void)
{
    return get_cur_bt_idx() & 0x03U;
}

static int link_key_tag_matches(u8 tag, u8 owner)
{
    return tag == BTSTACK_JL_LINK_KEY_TAG_LEGACY ||
           tag == link_key_tag_for(owner);
}

/* How long to wait before writing a staged link key to flash.  The pairing
 * this record came from finishes in well under 100 ms. */
#define BTSTACK_JL_LINK_KEY_WRITE_DELAY_MS 2000

/* Defined with the rest of the time helpers further down; the staging above
 * and the flush below both need it. */
static u32 jl_time_ms(void);

static const edr_init_cfg_t *edr_config;
static const u8 *edr_report_map;
static u16 edr_report_map_size;
static u32 edr_class_of_device;

static u8 stack_ready;
static u8 user_pairing_requested;
static u8 peer_address[6];
static u8 peer_address_valid;
static u8 hid_ready;
static u8 btstack_task_created;

static void (*hid_output_handler)(u8 *packet, u16 size, u16 channel);
/* Device ID record and GET_REPORT handler for the session being started; see
 * btstack_jl_hid_ext.h.  btstack_init() uses them once and puts the Switch
 * profile's back, so a session whose profile sets nothing (the LE ones) never
 * inherits the previous profile's. */
#define HID_DEFAULT_VENDOR  0x057eU
#define HID_DEFAULT_PRODUCT 0x2009U
#define HID_DEFAULT_VERSION 0x0001U
static u16 hid_device_id_vendor = HID_DEFAULT_VENDOR;
static u16 hid_device_id_product = HID_DEFAULT_PRODUCT;
static u16 hid_device_id_version = HID_DEFAULT_VERSION;
static int (*hid_get_report_handler)(u8 report_type, u8 report_id, u8 *report,
                                     u16 size);

static void jl_link_policy_apply(void);
static void (*hid_send_wakeup)(void);

/* ----------------------------------------------------------------- logging */

#define BTSTACK_JL_LOG(...) cyclone2_usb_debug("BTSTACK: " __VA_ARGS__)

/* Controller-library entry points used by this file. */
extern void lmp_set_sniff_establish_by_remote(u8 enable);
extern void lmp_sniff_t_slot_attemp_reset(u16 slot, u16 attemp);
extern void jieli_btctrler_get_local_address(uint8_t *address);
extern const int sniff_support_reset_anchor_point;

/* ------------------------------------------------------------- link keys */

#if CYCLONE2_BOND_STORE_RAM
/* Nothing to flush: the RAM store is written in place. */
void btstack_jl_link_key_flush(u8 force)
{
    (void)force;
}

/* CYCLONE2_BOND_STORE_RAM makes the bond store volatile: the records live in
 * internal RAM and are never written to syscfg.  Every boot (and every reflash)
 * then starts with no bonds at all, and pairing no longer costs a flash write.
 * A record that was never written reads back as empty because its tag byte is
 * zero, exactly like a blank syscfg item below. */
static u8 link_key_ram[BTSTACK_JL_LINK_KEY_ENTRIES][BTSTACK_JL_LINK_KEY_SIZE];

static int link_key_slot_read(u8 slot, u8 *record)
{
    if (slot >= BTSTACK_JL_LINK_KEY_ENTRIES) return 0;
    memcpy(record, link_key_ram[slot], BTSTACK_JL_LINK_KEY_SIZE);
    if (!link_key_tag_valid(record[0])) return 0;
    return 1;
}

static int link_key_slot_write(u8 slot, u8 owner, const u8 *address,
                               const u8 *key, u8 type)
{
    if (slot >= BTSTACK_JL_LINK_KEY_ENTRIES) return 0;
    link_key_ram[slot][0] = link_key_tag_for(owner);
    memcpy(&link_key_ram[slot][1], address, 6);
    memcpy(&link_key_ram[slot][7], key, 16);
    link_key_ram[slot][23] = type;
    return 1;
}

static void link_key_slot_clear(u8 slot)
{
    if (slot >= BTSTACK_JL_LINK_KEY_ENTRIES) return;
    memset(link_key_ram[slot], 0, BTSTACK_JL_LINK_KEY_SIZE);
}
#else
/* A persistent build does not touch syscfg from the callback the controller
 * uses for a derived link key: that runs inside the SSP exchange, and any
 * syscfg access there - a read while looking for the peer's slot as much as
 * the write - resets the pad.  Measured: the pairing reaches LINK KEY
 * NOTIFICATION and the chip resets immediately, while the RAM-only build
 * (which touches no flash on this path) goes on to encrypt and open the HID
 * channels.  The record is staged here and written by the application task
 * once the exchange is over. */
static u8 link_key_write_pending;
static u32 link_key_write_due_ms;
static u8 link_key_staged_address[6];
static u8 link_key_staged_key[16];
static u8 link_key_staged_type;
static u8 link_key_staged_owner;

/* Slots whose records have been dropped but not yet erased in flash.  The
 * deletes run while the radio is starting (forgetting a peer before a fresh
 * pairing, and the stack's "forget everything" step), and erasing there resets
 * the pad for the same reason the link-key write does. */
static u16 link_key_clear_pending;

static int link_key_slot_read(u8 slot, u8 *record)
{
#ifdef CYCLONE2_BOND_NO_KEY_FLASH
    /* Diagnostic: pretend the store is empty instead of reading syscfg. */
    (void)slot; (void)record;
    return 0;
#else
    int length = syscfg_read((u16)(CFG_REMOTE_DB_00 + slot), record,
                             BTSTACK_JL_LINK_KEY_SIZE);

    if (length != BTSTACK_JL_LINK_KEY_SIZE) return 0;
    if (!link_key_tag_valid(record[0])) return 0;
    return 1;
#endif
}

static int link_key_slot_write(u8 slot, u8 owner, const u8 *address,
                               const u8 *key, u8 type)
{
#ifdef CYCLONE2_BOND_NO_KEY_FLASH
    (void)slot; (void)owner; (void)address; (void)key; (void)type;
    return 1;
#else
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];

    record[0] = link_key_tag_for(owner);
    memcpy(record + 1, address, 6);
    memcpy(record + 7, key, 16);
    record[23] = type;
    {
        int written;

        cyclone2_bringup_record(0x3b00u | slot);
        written = syscfg_write((u16)(CFG_REMOTE_DB_00 + slot), record,
                               sizeof(record));
        cyclone2_bringup_record(0x3c00u | slot);
        return written == sizeof(record);
    }
#endif
}

static void link_key_slot_clear(u8 slot)
{
    /* Deferred: the erases happen while the radio is starting, and a flash
     * write there resets the pad (measured: `pair N` puts the pad in pairing
     * mode and the chip resets seconds later, with the boot counter moving). */
    if (slot < BTSTACK_JL_LINK_KEY_ENTRIES) {
        link_key_clear_pending |= (u16)(1U << slot);
    }
}
#endif /* CYCLONE2_BOND_STORE_RAM */

/* Where a record for (owner, address) goes: that owner's record for the
 * address, else a legacy record for it (the pairing supersedes it), else the
 * first empty slot, else slot 0.  Scans the whole store - an empty slot can
 * sit before the matching record. */
static u8 link_key_slot_for(u8 owner, const u8 address[6])
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 legacy = BTSTACK_JL_LINK_KEY_ENTRIES;
    u8 empty = BTSTACK_JL_LINK_KEY_ENTRIES;
    u8 slot;

    for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
        if (!link_key_slot_read(slot, record)) {
            if (empty == BTSTACK_JL_LINK_KEY_ENTRIES) empty = slot;
            continue;
        }
        if (memcmp(record + 1, address, 6) != 0) continue;
        if (record[0] == link_key_tag_for(owner)) return slot;
        if (record[0] == BTSTACK_JL_LINK_KEY_TAG_LEGACY &&
            legacy == BTSTACK_JL_LINK_KEY_ENTRIES) legacy = slot;
    }
    if (legacy != BTSTACK_JL_LINK_KEY_ENTRIES) return legacy;
    if (empty != BTSTACK_JL_LINK_KEY_ENTRIES) return empty;
    return 0;
}

static void link_key_store(u8 owner, const u8 address[6], const u8 key[16],
                           u8 type)
{
#if CYCLONE2_BOND_STORE_RAM
    u8 slot = link_key_slot_for(owner, address);

    link_key_slot_write(slot, owner, address, key, type);
    cyclone2_bringup_record(0x3400u | slot);
#else
    /* Staged, not stored: no syscfg access at all from here.  The slot is
     * chosen (and the write done) by btstack_jl_link_key_flush() from the
     * application task. */
    memcpy(link_key_staged_address, address, 6);
    memcpy(link_key_staged_key, key, 16);
    link_key_staged_type = type;
    link_key_staged_owner = owner;
    link_key_write_due_ms = jl_time_ms() + BTSTACK_JL_LINK_KEY_WRITE_DELAY_MS;
    link_key_write_pending = 1;
#endif
}

static int jl_link_key_get(const u8 address[6], u8 key[16], u8 *type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 owner = link_key_owner();
    u8 legacy = BTSTACK_JL_LINK_KEY_ENTRIES;
    u8 slot;

#if !CYCLONE2_BOND_STORE_RAM
    /* A key staged by a pairing is not in flash until the radio has been idle
     * for a while; a peer that reconnects before that must still find it. */
    if (link_key_write_pending &&
        memcmp(link_key_staged_address, address, 6) == 0 &&
        (link_key_staged_owner == owner ||
         link_key_staged_owner == BTSTACK_JL_LINK_KEY_ANY_SLOT)) {
        memcpy(key, link_key_staged_key, 16);
        *type = link_key_staged_type;
        return 1;
    }
#endif
    /* This slot's own record first; a legacy one only if there is none. */
    for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
        if (!link_key_slot_read(slot, record)) continue;
        if (memcmp(record + 1, address, 6) != 0) continue;
        if (record[0] == link_key_tag_for(owner)) break;
        if (record[0] == BTSTACK_JL_LINK_KEY_TAG_LEGACY &&
            legacy == BTSTACK_JL_LINK_KEY_ENTRIES) legacy = slot;
    }
    if (slot == BTSTACK_JL_LINK_KEY_ENTRIES) {
        if (legacy == BTSTACK_JL_LINK_KEY_ENTRIES) return 0;
        slot = legacy;
        link_key_slot_read(slot, record);
    }
    cyclone2_bringup_record(0x3300u | slot);
    {
        u8 xor_sum = 0U;
        for (u8 i = 0U; i < 16U; i++) xor_sum ^= record[7 + i];
        cyclone2_bringup_record(0x3800u | record[7]);
        cyclone2_bringup_record(0x3900u | record[22]);
        cyclone2_bringup_record(0x3a00u | xor_sum);
    }
    memcpy(key, record + 7, 16);
    *type = record[23];
    return 1;
}

static void jl_link_key_put(const u8 address[6], const u8 key[16], u8 type)
{
    link_key_store(link_key_owner(), address, key, type);
    cyclone2_bringup_record(0x3500u | key[0]);
    cyclone2_bringup_record(0x3600u | key[15]);
    {
        u8 xor_sum = 0U;
        for (u8 i = 0U; i < 16U; i++) xor_sum ^= key[i];
        cyclone2_bringup_record(0x3700u | xor_sum);
    }
}

/* Forget a host for the running slot only: its own record and any legacy one
 * for that host.  Other slots paired with the same host keep their keys. */
static void jl_link_key_delete(const u8 address[6])
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 owner = link_key_owner();
    u8 slot;

    for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
        if (!link_key_slot_read(slot, record)) continue;
        if (memcmp(record + 1, address, 6) != 0) continue;
        if (!link_key_tag_matches(record[0], owner)) continue;
        link_key_slot_clear(slot);
    }
}

/* BTstack's view of the store: the running slot's records and legacy ones. */
static int jl_link_key_iterator(u8 index, u8 address[6], u8 key[16], u8 *type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 owner = link_key_owner();
    u8 slot = index;
    u8 valid;

    while (slot < BTSTACK_JL_LINK_KEY_ENTRIES) {
        if (link_key_slot_read(slot, record) &&
            link_key_tag_matches(record[0], owner)) {
            valid = 0;
            for (u8 i = 0; i < 6; i++) valid |= record[1 + i];
            if (valid != 0) {
                memcpy(address, record + 1, 6);
                memcpy(key, record + 7, 16);
                *type = record[23];
                return 1;
            }
        }
        slot++;
    }
    return 0;
}

/* Console view of the same store, addressed by raw slot so an empty slot can
 * be told apart from a record that did not match.  See btstack_jl.h. */
int btstack_jl_link_key_store_read(u8 index, u8 address[6], u8 key[16], u8 *type)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];

    if (index >= BTSTACK_JL_LINK_KEY_ENTRIES) return 0;
    if (!link_key_slot_read(index, record)) return 0;
    memcpy(address, record + 1, 6);
    memcpy(key, record + 7, 16);
    *type = record[23];
    return 1;
}

void btstack_jl_link_key_store_write(u8 owner, const u8 address[6],
                                     const u8 key[16], u8 type)
{
    link_key_store(owner < 4U ? owner : BTSTACK_JL_LINK_KEY_ANY_SLOT,
                   address, key, type);
}

#if !CYCLONE2_BOND_STORE_RAM
/* Write a staged link key once the pairing that produced it is over.  Called
 * from the application task; see the note above link_key_slot_write().
 *
 * The writes are made with interrupts masked.  This pad executes out of the
 * same SPI flash the syscfg layer writes to, and an interrupt taken in the
 * middle of a flash program/erase takes its handler from a bus that is busy -
 * which is the most direct explanation for the resets this used to cause. */
void btstack_jl_link_key_flush(u8 force)
{
    u8 record[BTSTACK_JL_LINK_KEY_SIZE];
    u8 cleared[BTSTACK_JL_LINK_KEY_SIZE];
    u8 slot;

    /* Erase any records the pairing path asked to drop.  Same rule as the
     * write below: only while the radio is idle - the application gates this
     * call, and `force` is for the console commands, which run idle too. */
    if (link_key_clear_pending != 0U) {
        u16 pending = link_key_clear_pending;
        link_key_clear_pending = 0U;
        memset(cleared, 0, sizeof(cleared));
        local_irq_disable();
        for (slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
            if (pending & (u16)(1U << slot)) {
                cyclone2_bringup_record(0x3b80u | slot);
                syscfg_write((u16)(CFG_REMOTE_DB_00 + slot), cleared,
                             sizeof(cleared));
            }
        }
        local_irq_enable();
    }

    if (!link_key_write_pending) return;
    if (!force && (s32)(jl_time_ms() - link_key_write_due_ms) < 0) return;
    link_key_write_pending = 0;
#ifdef CYCLONE2_BOND_WRITE_DISABLED
    /* Diagnostic build: stage and drop, to tell a crash in the flash write
     * apart from one in the pairing path itself. */
    cyclone2_usb_debug("BTSTACK: link key dropped (write disabled)");
    return;
#endif
    /* This is the first flash access for this record: find its slot. */
    slot = link_key_slot_for(link_key_staged_owner, link_key_staged_address);
    local_irq_disable();
    link_key_slot_write(slot, link_key_staged_owner, link_key_staged_address,
                        link_key_staged_key, link_key_staged_type);
    local_irq_enable();
    cyclone2_bringup_record(0x3400u | slot);
}
#endif /* !CYCLONE2_BOND_STORE_RAM */

/* ----------------------------------------------------------------- events */

static u32 jl_time_ms(void)
{
    return sys_timer_get_ms();
}

static void jl_critical_enter(void)
{
    local_irq_disable();
}

static void jl_critical_exit(void)
{
    local_irq_enable();
}

static void jl_trace(u32 code)
{
    cyclone2_bringup_trace = (code << 16) | (cyclone2_boot_count & 0xffffU);
    cyclone2_bringup_record(code);
}

/* HIDP message from the host; hand it to the application's HID output handler
 * (switch_output() in profile_switch.c). */
static void jl_hid_output(const u8 *packet, u16 size)
{
    if (hid_output_handler != 0) {
        hid_output_handler((u8 *) packet, size, 0);
    }
}

static void bt_event_post(u32 arg_type, u8 event, const u8 *args, u32 value)
{
    struct sys_event e;

    e.type = SYS_BT_EVENT;
    e.arg = (void *) arg_type;
    e.u.bt.event = event;
    if (args != 0) {
        memcpy(e.u.bt.args, args, 7);
    } else {
        memset(e.u.bt.args, 0, sizeof(e.u.bt.args));
    }
    e.u.bt.value = value;
    sys_event_notify(&e);
}

/* Notifications from the BTstack task.  Runs in the "btstack" task context;
 * sys_event_notify() hands the event to the application task. */
static void jl_event_handler(const btstack_jl_event_t *event)
{
    switch (event->type) {
        case BTSTACK_JL_EVENT_READY:
            stack_ready = 1;
            jl_link_policy_apply();
            {
                /* Confirm the controller really carries the identity the
                 * application asked for: the BTIF copy of the address is only
                 * read at boot, so a changed address has to be written to the
                 * controller at runtime for a host to see a new device. */
                u8 controller_address[6];
                const u8 *wanted = bt_get_mac_addr();
                jieli_btctrler_get_local_address(controller_address);
                if (wanted != 0 &&
                    memcmp(controller_address, wanted, 6) != 0) {
                    BTSTACK_JL_LOG("controller address %02x:%02x:%02x:%02x:%02x:%02x != app %02x:%02x:%02x:%02x:%02x:%02x",
                                   controller_address[5], controller_address[4],
                                   controller_address[3], controller_address[2],
                                   controller_address[1], controller_address[0],
                                   wanted[5], wanted[4], wanted[3], wanted[2],
                                   wanted[1], wanted[0]);
                    cyclone2_bringup_record(0x2cu);
                }
            }
            BTSTACK_JL_LOG("stack ready");
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_INIT_OK, 0, 0);
            break;

        case BTSTACK_JL_EVENT_CONNECTED:
            memcpy(peer_address, event->address, sizeof(peer_address));
            peer_address_valid = 1;
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_FIRST_CONNECTED,
                          event->address, 0);
            break;

        case BTSTACK_JL_EVENT_ENCRYPTED:
            memcpy(peer_address, event->address, sizeof(peer_address));
            peer_address_valid = 1;
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_ENCRY_COMPLETE,
                          event->address, 0);
            break;

        case BTSTACK_JL_EVENT_DISCONNECTED:
            hid_ready = 0;
            bt_event_post(SYS_BT_EVENT_TYPE_CON_STATUS, BT_STATUS_FIRST_DISCONNECT,
                          event->address, event->status);
            break;

        case BTSTACK_JL_EVENT_HID_OPENED:
            hid_ready = 1;
            memcpy(peer_address, event->address, sizeof(peer_address));
            peer_address_valid = 1;
            BTSTACK_JL_LOG("hid channels open");
            break;

        case BTSTACK_JL_EVENT_HID_CLOSED:
            hid_ready = 0;
            BTSTACK_JL_LOG("hid channels closed status=0x%02x", event->status);
            break;

        case BTSTACK_JL_EVENT_HCI_STATUS:
            bt_event_post(SYS_BT_EVENT_TYPE_HCI_STATUS, event->code,
                          event->address, event->status);
            break;

        default:
            break;
    }
}

/* ---------------------------------------------------------- link policy */

extern void lmp_set_sniff_establish_by_remote(u8 enable);
extern void lmp_sniff_t_slot_attemp_reset(u16 slot, u16 attemp);
extern const int sniff_support_reset_anchor_point;

/* The console asks for a 24 slot (15 ms) sniff interval as soon as the HID
 * channels are open.  The controller library's default answer is 800 slots
 * (500 ms); the console retries the negotiation three times and then drops the
 * link.  This weak hook in btctrler.a has to be overridden with the profile's
 * parameters, which is what the vendor host library used to do. */
void sniff_negotiation_hook(u16 *T_sniff, u16 *attemp, u16 *timeout,
                            u8 negotiation)
{
    (void) negotiation;

    /* The console asks to enter sniff as soon as the HID channels open, and
     * that negotiation is the point where the controller changes its clock and
     * power state - worth having in the ring when a reset lands in this
     * window.  Recorded before the early returns so a hook call with no
     * parameters is visible too. */
    cyclone2_bringup_record(0x7000u);
    if ((T_sniff == 0) || (attemp == 0) || (timeout == 0)) {
        return;
    }
    if ((edr_config == 0) || (edr_config->sniff_param == 0)) {
        return;
    }
    *T_sniff = edr_config->sniff_param->max_interval_slots;
    *attemp = edr_config->sniff_param->attempt_slots;
    *timeout = edr_config->sniff_param->timeout_slots;
}

static void jl_link_policy_apply(void)
{
    /* A HID device must accept the console's sniff request. */
    cyclone2_bringup_record(0x2bu);
    lmp_set_sniff_establish_by_remote(1U);
    if ((sniff_support_reset_anchor_point != 0) && (edr_config != 0) &&
        (edr_config->sniff_param != 0)) {
        lmp_sniff_t_slot_attemp_reset(edr_config->sniff_param->max_interval_slots,
                                      edr_config->sniff_param->attempt_slots);
    }
}

/* ----------------------------------------------------------- stack task */

static void btstack_task(void *argument)
{
    (void) argument;

    cyclone2_bringup_record(6U);
    /* Never returns: the task parks between radio sessions. */
    btstack_jl_core_run(0);
}

/* ------------------------------------------------- vendor host API ---- */

void btstack_edr_start_before_init(const edr_init_cfg_t *cfg, int param)
{
    (void) param;

    cyclone2_bringup_record(1U);
    edr_config = cfg;
    edr_report_map = cfg->report_map;
    edr_report_map_size = cfg->report_map_size;
    edr_class_of_device = cfg->class_type;
    hid_ready = 0;
    stack_ready = 0;
    peer_address_valid = 0;
}

void btstack_init(void)
{
    btstack_jl_config_t config;
    const u8 *address = bt_get_mac_addr();

    cyclone2_bringup_record(2U);
    /* Route BTstack's own HCI trace and log messages to the USB console before
     * the stack starts talking to the controller. */
    cyclone2_hci_dump_init();
    memset(&config, 0, sizeof(config));
    config.local_name = bt_get_local_name();
    config.class_of_device = edr_class_of_device;
    if (address != 0) {
        memcpy(config.bd_addr, address, sizeof(config.bd_addr));
    }
    config.report_descriptor = edr_report_map;
    config.report_descriptor_size = edr_report_map_size;
    config.io_capability = edr_config != 0 ? edr_config->io_capabilities : 3;
    config.authentication_requirement = edr_config != 0 ? edr_config->authentication_req : 4;
    config.page_timeout_ms = edr_config != 0 ? edr_config->page_timeout : 8000;
    config.supervision_timeout_ms = edr_config != 0 ? edr_config->super_timeout : 8000;
    config.link_key_get = &jl_link_key_get;
    config.link_key_put = &jl_link_key_put;
    config.link_key_delete = &jl_link_key_delete;
    config.link_key_iterator = &jl_link_key_iterator;
    config.time_ms = &jl_time_ms;
    config.critical_enter = &jl_critical_enter;
    config.critical_exit = &jl_critical_exit;
    config.trace = &jl_trace;
    config.hid_output = &jl_hid_output;
    config.device_id_vendor = hid_device_id_vendor;
    config.device_id_product = hid_device_id_product;
    config.device_id_version = hid_device_id_version;
    config.hid_get_report = hid_get_report_handler;
    hid_device_id_vendor = HID_DEFAULT_VENDOR;
    hid_device_id_product = HID_DEFAULT_PRODUCT;
    hid_device_id_version = HID_DEFAULT_VERSION;
    hid_get_report_handler = 0;

    cyclone2_bringup_record(3U);
    BTSTACK_JL_LOG("init name=%s", config.local_name ? config.local_name : "?");

    if (btstack_jl_core_init(&config, &jl_event_handler) != 0) {
        BTSTACK_JL_LOG("init failed");
        return;
    }

    cyclone2_bringup_record(4U);
    if (!btstack_task_created) {
        BTSTACK_JL_LOG("creating task");
        task_create(btstack_task, NULL, "btstack");
        btstack_task_created = 1U;
    }
    btstack_jl_core_request_start();
    cyclone2_bringup_record(5U);
}

/* Teardown poll: non-blocking and idempotent.
 *
 * The HCI halt waits for the peer to confirm the disconnect, and for a live
 * link that can take as long as the supervision timeout (8 s in this profile).
 * Blocking the application task for that long stalls the USB console, and
 * returning early and letting the caller start the next session was what reset
 * the chip.  So the caller polls this from a timer instead: the first call asks
 * for the shutdown, later calls report whether the run-loop task has parked,
 * so the wait is bounded by the caller's own deadline rather than by a
 * forced exit.  Forcing the run loop out mid-halt skips the transport close
 * and leaves the controller half-shut, which the next session then has to
 * cope with - it produced resets when it was tried.
 *
 * Returns 1 once the session really is down. */
int btstack_edr_exit(int param)
{
    static u32 started_ms;
    static u8 active;
    static u8 forced;

    (void) param;

    if (!active) {
        active = 1;
        forced = 0;
        started_ms = sys_timer_get_ms();
        user_pairing_requested = 0;
        hid_ready = 0;
        stack_ready = 0;
        /* BTstack calls have to happen on the run-loop task; the task parks
         * itself once the controller is powered down. */
        btstack_jl_core_request_shutdown();
        return 0;
    }

    if (btstack_jl_core_idle()) {
        active = 0;
        cyclone2_bringup_record(0x4100u);
        cyclone2_bringup_record(0x4200u |
                                (u32)(sys_timer_get_ms() - started_ms > 0xffU ?
                                      0xffU : sys_timer_get_ms() - started_ms));
        return 1;
    }

    if (!forced && (sys_timer_get_ms() - started_ms) > 8000U) {
        /* Past the profile's supervision timeout the peer is not going to
         * confirm, so it is not a live link any more.  Drop it and end the run
         * loop; the caller keeps polling until the teardown finishes, so the
         * next session still starts on a clean stack. */
        forced = 1;
        cyclone2_bringup_record(0x4300u);
        btstack_jl_core_force_shutdown();
    }
    return 0;
}

/* Enable page and inquiry scan (pairing), or disable both (paging out). */
void bt_wait_phone_connect_control(u8 enable)
{
    user_pairing_requested = enable ? 1 : 0;
    btstack_jl_core_set_scan(enable ? 1 : 0, enable ? 1 : 0);
}

void bt_wait_phone_connect_control_ext(u8 inquiry_en, u8 page_scan_en)
{
    btstack_jl_core_set_scan(page_scan_en ? 1 : 0, inquiry_en ? 1 : 0);
}

/* Page the peer this controller was bonded to last. */
int bt_connect_phone_back_start(void)
{
    u8 address[6];
    u8 key[16];
    u8 type;
    u8 index;

    if (peer_address_valid) {
        return btstack_jl_core_hid_connect(peer_address) == 0 ? 1 : 0;
    }

    for (index = 0; index < BTSTACK_JL_LINK_KEY_ENTRIES; index++) {
        if (!jl_link_key_iterator(index, address, key, &type)) {
            break;
        }
        if (btstack_jl_core_hid_connect(address) == 0) {
            memcpy(peer_address, address, sizeof(peer_address));
            peer_address_valid = 1;
            return 1;
        }
    }
    return 0;
}

u32 user_send_cmd_prepare(USER_CMD_TYPE cmd, u16 param_len, u8 *param)
{
    switch (cmd) {
        case USER_CTRL_START_CONNEC_VIA_ADDR:
        case USER_CTRL_START_CONNEC_VIA_ADDR_MANUALLY:
            if (param_len < 6 || param == 0) return 2;
            memcpy(peer_address, param, sizeof(peer_address));
            peer_address_valid = 1;
            return btstack_jl_core_hid_connect(param) == 0 ? 0 : 1;

        case USER_CTRL_DISCONNECTION_HCI:
            return btstack_jl_core_disconnect();

        case USER_CTRL_HID_CONN:
            /* The console is the HID host: it opens the control (0x11) and
             * interrupt (0x13) channels itself once the link is encrypted, and
             * BTstack's HID device profile accepts them.  Opening a second set
             * from this side makes the connection fail, so the command is a
             * no-op. */
            return 0;

        case USER_CTRL_HID_DISCONNECT:
            return btstack_jl_core_disconnect();

        case USER_CTRL_HID_SEND_DATA: {
            hid_s_param_t *send = (hid_s_param_t *) param;
            if (param_len < sizeof(hid_s_param_t) || send == 0) return 2;
            /* data_ptr already carries the HIDP header and report id. */
            return btstack_jl_core_send_report(send->data_ptr, send->data_len);
        }

        case USER_CTRL_DEL_ALL_REMOTE_INFO:
            peer_address_valid = 0;
            hid_ready = 0;
            memset(peer_address, 0, sizeof(peer_address));
            btstack_jl_core_delete_all_link_keys();
            /* Only the running slot's own records: the pairing flow sends
             * this before every fresh pairing, and clearing the whole store
             * here dropped every other slot's bonds (other consoles, other
             * profiles on the same PC).  Legacy records stay; the peer being
             * re-paired has its own dropped by forget_peer. */
            {
                u8 record[BTSTACK_JL_LINK_KEY_SIZE];
                u8 owner_tag = link_key_tag_for(link_key_owner());

                for (u8 slot = 0; slot < BTSTACK_JL_LINK_KEY_ENTRIES; slot++) {
                    if (link_key_slot_read(slot, record) &&
                        record[0] == owner_tag) {
                        link_key_slot_clear(slot);
                    }
                }
            }
            return 0;

        case USER_CTRL_ALL_SNIFF_EXIT:
            /* The controller owns the sniff state machine. */
            return 0;

        default:
            return 1;
    }
}

/* Vendor controller-library hook: keep the link alive when a peer rejects a
 * stale link key so the same connection can fall back to a fresh pairing. */
extern void edr_vendor_keep_connect_for_keymiss(u8 enable);

int user_hid_send_data(u8 *buf, u32 len)
{
    return btstack_jl_core_send_report(buf, (u16) len);
}

void user_hid_disconnect(void)
{
    /* Leave sniff before the link is torn down: the halt that follows waits for
     * the peer to confirm the disconnect. */
    btstack_jl_core_request_unsniff();
    btstack_jl_core_disconnect();
}

void user_hid_init(void (*output_handler)(u8 *packet, u16 size, u16 channel))
{
    hid_output_handler = output_handler;
}

void user_hid_exit(void)
{
    hid_output_handler = 0;
    hid_send_wakeup = 0;
    hid_ready = 0;
}

void user_hid_enable(u8 enable)
{
    if (!enable) {
        user_hid_disconnect();
    }
}

void user_hid_set_icon(u32 class_type)
{
    edr_class_of_device = class_type;
}

void user_hid_set_ReportMap(u8 *map, u16 size)
{
    edr_report_map = map;
    edr_report_map_size = size;
}

void user_hid_regiser_wakeup_send(void *cbk)
{
    hid_send_wakeup = (void (*)(void)) cbk;
}

void btstack_jl_hid_set_device_id(u16 vendor, u16 product, u16 version)
{
    hid_device_id_vendor = vendor;
    hid_device_id_product = product;
    hid_device_id_version = version;
}

void btstack_jl_hid_set_get_report_handler(
    int (*handler)(u8 report_type, u8 report_id, u8 *report, u16 size))
{
    hid_get_report_handler = handler;
}

int edr_hid_data_send_ext(u8 report_type, u8 report_id, u8 *data, u16 len)
{
    /* HIDP header + report id + body; the core queues at most 80 bytes. */
    u8 message[80];

    if (len + 2 > sizeof(message)) {
        return 2;
    }
    message[0] = report_type;
    message[1] = report_id;
    memcpy(message + 2, data, len);
    return btstack_jl_core_send_report(message, (u16)(len + 2));
}

int edr_hid_data_send(u8 report_id, u8 *data, u16 len)
{
    /* Wake the link before handing over a report, exactly where the SDK's HID
     * examples do it. */
    bt_comm_edr_sniff_clean();
    return edr_hid_data_send_ext(0xa1U, report_id, data, len);
}

int edr_hid_is_connected(void)
{
    return btstack_jl_core_hid_connected();
}

/* A report is still waiting for the link.  A profile that streams its whole
 * state can skip a poll while this is true, so the host always gets the
 * newest state instead of a backlog of old ones. */
int edr_hid_is_busy(void)
{
    return btstack_jl_core_reports_queued() != 0;
}

int edr_hid_tx_buff_is_ok(void)
{
    return 1;
}

/* The application still calls this before it starts a session; the controller
 * in this build keeps keys itself, so only the host bookkeeping is affected. */
void delete_link_key(bd_addr_t *address, u8 index)
{
    (void) index;
    if (address == 0) return;
    jl_link_key_delete((const u8 *) address);
    btstack_jl_core_delete_link_key((const u8 *) address);
}

/* The vendor stack emulated the console's own event handling here; keep the
 * symbols so the profile code keeps compiling, they only trace now. */
int bt_comm_edr_status_event_handler(struct bt_event *bt)
{
    (void) bt;
    return 0;
}

int bt_comm_edr_hci_event_handler(struct bt_event *bt)
{
    (void) bt;
    return 0;
}

/* The SDK's own HID examples call this immediately before every
 * edr_hid_data_send() - "leave sniff, I have data".  It used to be a stub, so
 * this port kept pushing reports into a link the console was holding in sniff:
 * the controller accepted nothing between anchors, its ACL buffers filled, and
 * the chip reset a few hundred ms later.  The request is debounced and only
 * acted on while the link really is in sniff (see the core). */
int bt_comm_edr_sniff_clean(void)
{
    btstack_jl_core_request_unsniff();
    return 1;
}

void set_multi_devices_adv_flag(u8 flag)
{
    (void) flag;
}

/* BTstack asserts through the C library's assert(); the Jieli runtime has no
 * __assert_func, so trap here where the console can show the failure. */
void __assert_func(const char *file, int line, const char *function,
                   const char *expression)
{
    cyclone2_usb_debug("BTSTACK assert %s:%d %s: %s", file, line,
                       function != 0 ? function : "?", expression);
    for (;;) {
    }
}

u8 btstack_jl_is_ready(void)
{
    return stack_ready;
}

u8 btstack_jl_hid_is_ready(void)
{
    return hid_ready;
}
