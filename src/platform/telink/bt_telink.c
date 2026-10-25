#include "sdk.h"
#include "board.h"

/* Bluetooth LE on the B80: pad.h's bt_ module on Telink's BLE stack, for the
 * LE profiles.  A profile's GATT database is BTstack's (compile_gatt.py), so
 * the same profile runs on either stack: bt_start() turns it into the
 * stack's attribute table.  Its constant values stay where they are (flash);
 * DYNAMIC ones get RAM, filled once from the profile's read(), which returns
 * only constant data; CCCs and the input report are kept here.  Writes other
 * than to a CCC are not passed to the profile yet.
 *
 * Pairing is LE legacy Just Works.  The stack keeps up to 4 bonds of its own
 * in flash at BOND_STORE (two sectors); the slot's bond (struct bt_bond, in
 * the settings) is what counts: a pairing's record is caught on its way to
 * the stack's storage (--wrap=bls_smp_param_saveBondingInfo, a function the
 * SDK's headers leave out) and goes up as BT_EV_PAIRED, and bt_start() puts
 * the slot's back into the stack's storage when the stack has lost it.
 * Discoverable only while pairing.  One thread: the platform's wait loop
 * runs the stack (plat_bt_poll), whose callbacks only note what happened
 * for bt_poll(). */

#define BOND_STORE    0x71000    /* below the app's end, clear of stock data */
#define ATTRS         32         /* the largest profile database's last handle + 1 */
#define DYNAMIC_RAM   96         /* bytes for DYNAMIC values */
#define INPUT_MAX     20         /* a notification in a 27-byte LL packet */
#define IN_FLIGHT     2          /* input notifications in the stack at once */
/* The SDK accepts a notification only when queued + its packet count is
 * strictly below TX_FIFO - 2. Four entries allow only one pending input
 * packet, which waits through acknowledgement and halves the report rate.
 * Eight entries leave room for IN_FLIGHT inputs plus the SDK's reserve. */
#define TX_FIFO       8          /* LL packets queued to send (2^n) */
_Static_assert(TX_FIFO >= IN_FLIGHT + 3, "TX FIFO must fit inputs and the SDK reserve");

/* BTstack's attribute flags (att_db.h, bluetooth.h). */
#define BTS_READ         0x0002
#define BTS_WRITE_NR     0x0004
#define BTS_WRITE        0x0008
#define BTS_DYNAMIC      0x0100
#define BTS_UUID128      0x0200
#define BTS_READ_SEC     0x0c00
#define BTS_WRITE_SEC    0x0011

static u8 acl_rx[CAL_LL_ACL_RX_FIFO_SIZE(27) * 4] __attribute__((aligned(16)));
static u8 acl_tx[CAL_LL_ACL_TX_FIFO_SIZE(27) * TX_FIFO] __attribute__((aligned(4)));
static u8 l2cap_rx[CAL_L2CAP_BUFF_SIZE(23)] __attribute__((aligned(4)));

static attribute_t attrs[ATTRS];
static u8 dynamic[DYNAMIC_RAM] __attribute__((aligned(4)));
static const u8 filler_uuid[2] = { 0xff, 0xff };    /* a handle the database leaves out */

/* The stack's own, from smp_storage.o: a bond record into its storage. */
#define BOND_JUST_WORKS 0x9a     /* the record's flag after an unauthenticated pairing */
u32 __real_bls_smp_param_saveBondingInfo(smp_param_save_t *p);

static struct {
    u8 started, advertising, connected, encrypted;
    u8 linked_pending, unlinked_pending, paired_pending, reason;
    u8 stopping, stopped_pending;
    struct bt_bond paired;       /* the last pairing's, for BT_EV_PAIRED */
    u8 *input, *input_ccc;
    u16 input_handle;
    const struct bt_device *device;
} b;

static u16 get16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

/* The profile's database as the stack's table; 0: it does not fit. */
static int build(const struct profile *p)
{
    const u8 *e = p->device->att_db + 1;     /* after the version byte */
    u32 used = 0, last = 0, i, n;

    memset(attrs, 0, sizeof(attrs));
    memset(dynamic, 0, sizeof(dynamic));
    b.input = b.input_ccc = NULL;
    for (; get16(e); e += get16(e)) {
        u16 size = get16(e), flags = get16(e + 2), handle = get16(e + 4);
        u8 uuid_len = flags & BTS_UUID128 ? 16 : 2;
        const u8 *value = e + 6 + uuid_len;
        u32 value_len = size - 6 - uuid_len;
        attribute_t *a;

        if (handle >= ATTRS || handle <= last) return 0;
        for (last++; last < handle; last++) {
            attrs[last].perm = ATT_PERMISSIONS_READ;
            attrs[last].uuidLen = 2;
            attrs[last].uuid = (u8 *)filler_uuid;
        }
        a = &attrs[handle];
        a->uuidLen = uuid_len;
        a->uuid = (u8 *)e + 6;
        a->perm = (flags & BTS_READ ? ATT_PERMISSIONS_READ : 0) |
                  (flags & (BTS_WRITE | BTS_WRITE_NR) ? ATT_PERMISSIONS_WRITE : 0) |
                  (flags & (BTS_READ_SEC | BTS_WRITE_SEC) ? ATT_PERMISSIONS_ENCRYPT : 0);
        if (!(flags & BTS_DYNAMIC)) {
            a->pAttrValue = (u8 *)value;
            a->attrLen = value_len;
            continue;
        }
        /* RAM: the CCC, the input report, or what read() answers (else 4 zero bytes). */
        n = uuid_len == 2 && get16(a->uuid) == GATT_UUID_CLIENT_CHAR_CFG ? 2 :
            handle == p->device->input_handle ? INPUT_MAX : 4;
        if (used + n > sizeof(dynamic)) return 0;
        a->pAttrValue = dynamic + used;
        a->attrLen = n;
        if (handle == p->device->input_handle) {
            b.input = a->pAttrValue;
            a->attrLen = 0;
        } else if (n == 4 && p->read) {
            int got = p->read(BT_WRITE_ATT, handle, 0, dynamic + used,
                              (u16)(sizeof(dynamic) - used));

            if (got >= 0) a->attrLen = n = (u32)got;
        }
        if (handle == p->device->input_handle + 1) b.input_ccc = a->pAttrValue;
        used += (n + 3) & ~3u;
    }
    attrs[0].attNum = (u16)last;
    /* A service declaration holds how many attributes the service has. */
    for (i = 1; i <= last; i++) {
        if (attrs[i].uuidLen != 2 || (get16(attrs[i].uuid) != GATT_UUID_PRIMARY_SERVICE &&
                                      get16(attrs[i].uuid) != GATT_UUID_SECONDARY_SERVICE))
            continue;
        for (n = i + 1; n <= last; n++)
            if (attrs[n].uuidLen == 2 && (get16(attrs[n].uuid) == GATT_UUID_PRIMARY_SERVICE ||
                                          get16(attrs[n].uuid) == GATT_UUID_SECONDARY_SERVICE))
                break;
        attrs[i].attNum = (u16)(n - i);
    }
    b.input_handle = p->device->input_handle;
    return b.input && b.input_ccc;
}

static void on_connect(u8 e, u8 *p, int n)
{
    (void)e;
    (void)p;
    (void)n;
    b.connected = 1;
    b.encrypted = 0;
    b.advertising = 0;
    if (b.input_ccc) b.input_ccc[0] = b.input_ccc[1] = 0;
    if (b.stopping || !b.device) {
        b.stopping = 1;
        b.stopped_pending = 0;
        bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }
    bls_l2cap_requestConnParamUpdate(b.device->interval_min, b.device->interval_max, 0, 400);
    util_log("bt: connected");
}

static void on_terminate(u8 e, u8 *p, int n)
{
    (void)e;
    (void)n;
    b.connected = b.encrypted = 0;
    if (b.input_ccc) b.input_ccc[0] = b.input_ccc[1] = 0;
    b.reason = *p;
    b.unlinked_pending = 1;
    if (b.stopping) {
        b.stopping = 0;
        b.stopped_pending = 1;
    }
    util_log("bt: disconnected, reason %02x", *p);
}

static int on_host(u32 h, u8 *p, int n)
{
    (void)p;
    (void)n;
    switch (h & 0xff) {
    case GAP_EVT_SMP_PAIRING_SUCCESS:
        util_log("bt: paired");
        break;
    case GAP_EVT_SMP_PAIRING_FAIL:
        util_log("bt: pairing failed");
        break;
    case GAP_EVT_SMP_CONN_ENCRYPTION_DONE:
        b.encrypted = 1;
        if (!b.stopping) b.linked_pending = 1;
        util_log("bt: encrypted");
        break;
    }
    return 0;
}

static int is_zero(const u8 *p, u32 n)
{
    while (n--) if (*p++) return 0;
    return 1;
}

/* A pairing's record on its way into the stack's storage: the slot's bond. */
u32 __wrap_bls_smp_param_saveBondingInfo(smp_param_save_t *p)
{
    struct bt_bond *k = &b.paired;

    memset(k, 0, sizeof(*k));
    memcpy(k->le_peer, p->peer_addr, 6);
    memcpy(k->ltk, p->own_ltk, 16);
    memcpy(k->le_irk, p->peer_irk, 16);
    memcpy(k->le_id, p->peer_id_addr, 6);
    k->le_id_type = p->peer_id_adrType;
    /* Without identity-key distribution the SDK leaves the identity empty.
     * Keep the connection address and its type so a public host is not
     * restored as a random one.  No IRK means there is no RPA to resolve. */
    if (is_zero(k->le_id, 6) || k->le_id_type > 1) {
        memcpy(k->le_id, p->peer_addr, 6);
        k->le_id_type = p->peer_addr_type;
    }
    b.paired_pending = 1;
    return __real_bls_smp_param_saveBondingInfo(p);
}

/* The slot's bond into the stack's storage, unless it has it already.  The
 * host's own address type is not kept: random unless it is its identity. */
static void restore(const struct bt_bond *k)
{
    smp_param_save_t r;
    u8 type;

    if (!k || is_zero(k->ltk, 16) || is_zero(k->le_peer, 6)) return;
    type = !is_zero(k->le_id, 6) && !memcmp(k->le_id, k->le_peer, 6) ? k->le_id_type : 1;
    if (bls_smp_param_loadByAddr(type, (u8 *)k->le_peer, &r) && !memcmp(r.own_ltk, k->ltk, 16)) return;
    memset(&r, 0, sizeof(r));
    r.flag = BOND_JUST_WORKS;
    r.peer_addr_type = type;
    memcpy(r.peer_addr, k->le_peer, 6);
    r.peer_id_adrType = k->le_id_type;
    memcpy(r.peer_id_addr, k->le_id, 6);
    memcpy(r.own_ltk, k->ltk, 16);
    memcpy(r.peer_irk, k->le_irk, 16);
    __real_bls_smp_param_saveBondingInfo(&r);
    util_log("bt: bond back into the stack");
}

/* At start-up, before the board's pins and ADC: the stack's set-up undoes
 * an ADC set up before it (its DMA never completes). */
void plat_bt_init(void)
{
    u8 address[6];

    memcpy(address, plat_unique_id(), 6);    /* bt_start() sets the session's */
    address[5] |= 0xc0;
    random_generator_init();
    blc_ll_initBasicMCU();
    blc_ll_initStandby_module(address);
    blc_ll_initAdvertising_module();
    blc_ll_initSlaveRole_module();
    blc_ll_setRandomAddr(address);
    blc_ll_setAclConnMaxOctetsNumber(27, 27);
    blc_ll_initAclConnTxFifo(acl_tx, CAL_LL_ACL_TX_FIFO_SIZE(27), TX_FIFO);
    blc_ll_initAclConnRxFifo(acl_rx, CAL_LL_ACL_RX_FIFO_SIZE(27), 4);
    blc_gap_peripheral_init();
    bls_att_setAttributeTable((u8 *)attrs);
    blc_att_setRxMtuSize(23);
    blc_l2cap_register_handler(blc_l2cap_packet_receive);
    blc_l2cap_initRxDataBuffer(l2cap_rx, sizeof(l2cap_rx));
    bls_smp_configPairingSecurityInfoStorageAddr(BOND_STORE);
    blc_smp_param_setBondingDeviceMaxNumber(4);
    blc_smp_peripheral_init();
    blc_smp_setSecurityLevel(Unauthenticated_Pairing_with_Encryption);
    blc_gap_registerHostEventHandler(on_host);
    blc_gap_setEventMask(GAP_EVT_MASK_SMP_PAIRING_SUCCESS | GAP_EVT_MASK_SMP_PAIRING_FAIL |
                         GAP_EVT_MASK_SMP_CONN_ENCRYPTION_DONE);
    bls_app_registerEventCallback(BLT_EV_FLAG_CONNECT, on_connect);
    bls_app_registerEventCallback(BLT_EV_FLAG_TERMINATE, on_terminate);
    if (blc_contr_checkControllerInitialization() || blc_host_checkHostInitialization())
        util_log("bt: stack set-up failed");
    rf_set_power_level_index(RF_POWER_P2p87dBm);
    b.started = 1;
}

/* Advertises as the profile asks: discoverable while pairing.  link is not
 * used yet. */
void bt_start(const struct profile *p, const u8 address[6], const struct bt_bond *bond,
              int pairing, const struct bt_link *link)
{
    u8 adv[31], n;

    (void)link;
    if (p->device->transport != BT_LE || !build(p)) {
        util_log("bt: %s does not fit", p->name);
        return;
    }
    if (!b.started) return;
    if (b.advertising) bls_ll_setAdvEnable(BLC_ADV_DISABLE);
    b.device = p->device;
    blc_ll_setRandomAddr((u8 *)address);
    restore(bond);
    n = p->device->advertising(bond, pairing, 0, adv);
    bls_ll_setAdvParam(ADV_INTERVAL_30MS, ADV_INTERVAL_35MS, ADV_TYPE_CONNECTABLE_UNDIRECTED,
                       OWN_ADDRESS_RANDOM, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
    bls_ll_setAdvData(adv, n);
    bls_ll_setScanRspData((u8 *)p->device->scan_response, p->device->scan_response_len);
    bls_ll_setAdvEnable(BLC_ADV_ENABLE);
    b.advertising = 1;
    util_log("bt: advertising %s, %u bonds", p->name, blc_smp_param_getCurrentBondingDeviceNumber());
}

void bt_stop(void)
{
    if (!b.started) return;
    b.stopping = b.connected;
    b.linked_pending = 0;
    if (b.connected) bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
    else b.stopped_pending = 1;
    bls_ll_setAdvEnable(BLC_ADV_DISABLE);
    b.advertising = 0;
    b.device = NULL;             /* no session: nothing advertises again */
}

/* After pairing, a lost link advertises the same session without the
 * discoverable flag. Advertising data may be updated while connected. */
void bt_update_bond(const struct bt_bond *bond)
{
    u8 adv[31], n;

    if (!b.device || b.stopping) return;
    n = b.device->advertising(bond, 0, 0, adv);
    bls_ll_setAdvData(adv, n);
}

/* No pairing or link set-up under way: a flash write is safe. */
int bt_radio_quiet(void)
{
    return !b.connected || b.encrypted;
}

int bt_input_wanted(void)
{
    return b.connected && b.encrypted && b.input_ccc && (b.input_ccc[0] & 1) &&
           blc_ll_getTxFifoNumber() < IN_FLIGHT;
}

/* The report without its id: on LE the report reference names it. */
int bt_send_input(u8 id, const u8 *data, u16 len)
{
    (void)id;
    if (!bt_input_wanted() || len > INPUT_MAX) return 0;
    memcpy(b.input, data, len);
    attrs[b.input_handle].attrLen = len;
    return blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, b.input_handle, (u8 *)data, len) == BLE_SUCCESS;
}

int bt_poll(struct bt_ev *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (b.linked_pending) {
        b.linked_pending = 0;
        ev->kind = BT_EV_LINKED;
        return 1;
    }
    if (b.paired_pending) {
        b.paired_pending = 0;
        ev->kind = BT_EV_PAIRED;
        ev->bond = b.paired;
        return 1;
    }
    if (b.unlinked_pending) {
        b.unlinked_pending = 0;
        ev->kind = BT_EV_UNLINKED;
        ev->reason = b.reason;
        ev->lost = b.reason == HCI_ERR_CONN_TIMEOUT;
        return 1;
    }
    if (b.stopped_pending) {
        b.stopped_pending = 0;
        ev->kind = BT_EV_STOPPED;
        return 1;
    }
    return 0;
}

/* From the wait loop: the stack runs; EV_BT when bt_poll() has news.  After
 * a disconnect the stack stops advertising; it starts again here. */
u32 plat_bt_poll(void)
{
    if (!b.started) return 0;
    blc_sdk_main_loop();
    if (!b.connected && !b.advertising && b.device) {
        bls_ll_setAdvEnable(BLC_ADV_ENABLE);
        b.advertising = 1;
    }
    return b.linked_pending || b.unlinked_pending || b.paired_pending || b.stopped_pending ? EV_BT : 0;
}
