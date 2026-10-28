#include "sdk.h"
#include "board.h"
#include "att_telink.h"

/* Bluetooth LE on the B80: Telink owns the controller, L2CAP and SMP.
 * Unmodified BTstack att_db.c serves each profile's database from flash;
 * att_telink.c keeps the per-connection CCCs and bounded outgoing PDUs.
 * Host writes are delivered to the control loop through bt_poll().
 *
 * Generic BLE pairing is LE legacy Just Works.  The stack keeps up to 4 bonds of its own
 * in flash at BOND_STORE (two sectors); the slot's bond (struct bt_bond, in
 * the settings) is what counts: a pairing's record is caught on its way to
 * the stack's storage (--wrap=bls_smp_param_saveBondingInfo, a function the
 * SDK's headers leave out) and goes up as BT_EV_PAIRED, and bt_start() puts
 * the slot's back into the stack's storage when the stack has lost it.
 * Discoverable only while pairing.  One thread: the platform's wait loop
 * runs the stack (plat_bt_poll), whose callbacks only note what happened
 * for bt_poll(). */

#define BOND_STORE    0x71000    /* below the app's end, clear of stock data */
#define INPUT_MAX     77         /* DualSense's extended report */
#define TX_OCTETS     TELINK_TX_OCTETS
#define RX_OCTETS     251
#define IN_FLIGHT     2          /* input notifications in the stack at once */
/* Inputs take at most IN_FLIGHT slots, so two stay for LL control: two
 * one-packet inputs (every input fits TX_OCTETS), or one in two fragments
 * if one ever does not.  The app owns
 * input admission; raw ATT sends use the SDK's priority path rather than
 * its conservative notification reserve (only one input with four slots). */
#define TX_FIFO       4
_Static_assert(TX_FIFO >= IN_FLIGHT + 2, "TX FIFO must reserve two control packets");

static u8 acl_rx[CAL_LL_ACL_RX_FIFO_SIZE(RX_OCTETS) * 4] __attribute__((aligned(16)));
static u8 acl_tx[CAL_LL_ACL_TX_FIFO_SIZE(TX_OCTETS) * TX_FIFO] __attribute__((aligned(4)));
static u8 l2cap_rx[CAL_L2CAP_BUFF_SIZE(TELINK_ATT_BUFFER)] __attribute__((aligned(4)));
/* The SDK's initialization checks still want a table. ATT requests go to
 * the flash server instead of this empty constant table. */
static const attribute_t sdk_attrs[1];

/* Exported by the SDK archive, absent from its headers. The SDK's own ATT
 * dispatcher uses this exact header-plus-payload call to send responses. */
ble_sts_t blc_l2cap_pushData_2_controller(u16 handle, u16 cid, u8 *header,
                                        int header_len, u8 *payload, int len);

/* The stack's own, from smp_storage.o: a bond record into its storage. */
#define BOND_JUST_WORKS 0x9a     /* the record's flag after an unauthenticated pairing */
u32 __real_bls_smp_param_saveBondingInfo(smp_param_save_t *p);
int bls_smp_getLtkReq(u16 handle, u8 *random, u16 ediv);
void blc_ll_registerLtkReqEvtCb(int (*callback)(u16, u8 *, u16));
ble_sts_t blc_hci_ltkRequestReply(u16 handle, u8 *key);
ble_sts_t blc_hci_ltkRequestNegativeReply(u16 handle);

void plat_l2cap_reset(void);
int plat_l2cap_receive(u16 handle, u8 *packet);
void plat_ll_allow_short_interval(int allow);
int plat_ll_requested_interval(u16 *interval);

static struct {
    u8 started, advertising, connected, encrypted;
    u8 linked_pending, unlinked_pending, paired_pending, reason;
    u8 stopping, stopped_pending;
    struct bt_bond paired;       /* the last pairing's, for BT_EV_PAIRED */
    u8 write_pending, wake;
    struct bt_write write;
    const struct bt_device *device;
} b;

static u16 get16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

/* L2CAP has already reassembled the PDU here. Keep the SDK's SMP and
 * signalling paths intact and replace only ATT (--wrap=l2cap_att_handler). */
u8 *__wrap_l2cap_att_handler(u16 handle, u8 *packet)
{
    plat_att_receive(packet + 6, get16(packet + 2));
    blc_att_setEffectiveMtuSize(handle, plat_att_mtu());
    if (packet[6] == 2) util_log("bt: ATT MTU %u", plat_att_mtu());
    return NULL;
}

int plat_att_transmit(const u8 *pdu, u16 len, int response)
{
    if (!b.connected || !len || (!response && blc_ll_getTxFifoNumber() >= IN_FLIGHT)) return 0;
    return blc_l2cap_pushData_2_controller((u16)(BLS_CONN_HANDLE | 0x8000),
               4, (u8 *)pdu, 1, (u8 *)pdu + 1, len - 1) == BLE_SUCCESS;
}

int plat_att_write(u16 handle, const u8 *data, u16 len)
{
    if (b.write_pending || len > sizeof(b.write.data)) return 0;
    b.write.kind = BT_WRITE_ATT;
    b.write.id = handle;
    b.write.len = len;
    memcpy(b.write.data, data, len);
    b.write_pending = 1;
    return 1;
}

static void on_connect(u8 e, u8 *p, int n)
{
    (void)e;
    (void)n;
    if (b.device && !b.device->sm_er) memcpy(b.paired.le_peer, p, 6);
    b.connected = 1;
    b.encrypted = 0;
    b.advertising = 0;
    plat_att_connected(BLS_CONN_HANDLE);
    plat_l2cap_reset();
    if (b.stopping || !b.device) {
        b.stopping = 1;
        b.stopped_pending = 0;
        bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }
    if (b.device->interval_min)
        bls_l2cap_requestConnParamUpdate(b.device->interval_min, b.device->interval_max, 0, 400);
    if (!b.device->sm_er) {
        b.linked_pending = 1;   /* proprietary pairing uses unencrypted ATT */
        blc_ll_setPhy(BLS_CONN_HANDLE, PHY_TRX_PREFER, PHY_PREFER_2M, PHY_PREFER_2M);
        blc_ll_exchangeDataLength(0x14, 251);
    }
    if (b.wake) {
        b.wake = 0;
        bt_update_bond(&b.paired);
    }
    util_log("bt: connected");
}

static void on_terminate(u8 e, u8 *p, int n)
{
    (void)e;
    (void)n;
    b.connected = b.encrypted = 0;
    plat_att_connected(BLS_CONN_HANDLE);
    plat_l2cap_reset();
    b.reason = *p;
    b.unlinked_pending = 1;
    if (b.stopping) {
        b.stopping = 0;
        b.stopped_pending = 1;
    }
    util_log("bt: disconnected, reason %02x", *p);
}

static void on_phy(u8 e, u8 *p, int n)
{
    (void)e;
    if (n >= 2) util_log("bt: PHY %u/%u", p[0], p[1]);
}

static void on_length(u8 e, u8 *p, int n)
{
    (void)e;
    if (n >= 4) util_log("bt: DLE rx %u tx %u", get16(p), get16(p + 2));
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
        plat_att_encrypted(1);
        if (!b.stopping && b.device && b.device->sm_er) b.linked_pending = 1;
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

/* Switch 2 distributes its key over ATT, outside SMP. Its profile stores
 * the AES key most significant byte first; the SDK controller takes LE. */
static int on_ltk(u16 handle, u8 *random, u16 ediv)
{
    u8 key[16];
    u32 i;
    if (!b.device || b.device->sm_er) return bls_smp_getLtkReq(handle, random, ediv);
    if (is_zero(b.paired.ltk, 16)) { blc_hci_ltkRequestNegativeReply(handle); return 0; }
    for (i = 0; i < 16; i++) key[i] = b.paired.ltk[15 - i];
    blc_hci_ltkRequestReply(handle, key);
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
    blc_ll_init2MPhy_feature();
    blc_ll_setRandomAddr(address);
    blc_ll_setAclConnMaxOctetsNumber(RX_OCTETS, TX_OCTETS);
    blc_ll_initAclConnTxFifo(acl_tx, CAL_LL_ACL_TX_FIFO_SIZE(TX_OCTETS), TX_FIFO);
    blc_ll_initAclConnRxFifo(acl_rx, CAL_LL_ACL_RX_FIFO_SIZE(RX_OCTETS), 4);
    blc_gap_peripheral_init();
    bls_att_setAttributeTable((u8 *)sdk_attrs);
    blc_att_setRxMtuSize(TELINK_ATT_BUFFER);
    blc_l2cap_register_handler(plat_l2cap_receive);
    blc_l2cap_initRxDataBuffer(l2cap_rx, sizeof(l2cap_rx));
    bls_smp_configPairingSecurityInfoStorageAddr(BOND_STORE);
    blc_smp_param_setBondingDeviceMaxNumber(4);
    blc_smp_peripheral_init();
    blc_ll_registerLtkReqEvtCb(on_ltk);
    blc_smp_setSecurityLevel(Unauthenticated_Pairing_with_Encryption);
    blc_gap_registerHostEventHandler(on_host);
    blc_gap_setEventMask(GAP_EVT_MASK_SMP_PAIRING_SUCCESS | GAP_EVT_MASK_SMP_PAIRING_FAIL |
                         GAP_EVT_MASK_SMP_CONN_ENCRYPTION_DONE);
    bls_app_registerEventCallback(BLT_EV_FLAG_CONNECT, on_connect);
    bls_app_registerEventCallback(BLT_EV_FLAG_TERMINATE, on_terminate);
    bls_app_registerEventCallback(BLT_EV_FLAG_PHY_UPDATE, on_phy);
    bls_app_registerEventCallback(BLT_EV_FLAG_DATA_LENGTH_EXCHANGE, on_length);
    if (blc_contr_checkControllerInitialization() || blc_host_checkHostInitialization())
        util_log("bt: stack set-up failed");
    /* FIFO bounds were validated at our actual fragment size. The LL can
     * negotiate 251; the app caps its generated PDUs and guards SDK writes. */
    blc_ll_setAclConnMaxOctetsNumber(RX_OCTETS, 251);
    blc_ll_initAclConnTxFifo(acl_tx, CAL_LL_ACL_TX_FIFO_SIZE(TX_OCTETS), TX_FIFO);
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
    if (p->device->transport != BT_LE || !plat_att_start(p, p->device->sm_er ? TELINK_ATT_BUFFER : 512)) {
        util_log("bt: %s does not fit", p->name);
        return;
    }
    if (!b.started) return;
    if (b.advertising) bls_ll_setAdvEnable(BLC_ADV_DISABLE);
    b.device = p->device;
    plat_ll_allow_short_interval(!b.device->sm_er);
    /* The shared Switch 2 transport uses a public identity. Keep the SDK's
     * public address in step with the slot as well as its random address;
     * sessions are fully stopped before this initialization runs. */
    blc_ll_initStandby_module((u8 *)address);
    blc_ll_setRandomAddr((u8 *)address);
    b.paired = *bond;
    b.write_pending = b.wake = 0;
    blc_smp_setSecurityLevel(b.device->sm_er ? Unauthenticated_Pairing_with_Encryption : No_Security);
    blc_smp_configSecurityRequestSending(b.device->sm_er ? SecReq_IMM_SEND : SecReq_NOT_SEND,
                                        b.device->sm_er ? SecReq_IMM_SEND : SecReq_NOT_SEND, 0);
    if (b.device->sm_er) restore(bond);
    n = p->device->advertising(bond, pairing, 0, adv);
    bls_ll_setAdvParam(ADV_INTERVAL_30MS, ADV_INTERVAL_35MS, ADV_TYPE_CONNECTABLE_UNDIRECTED,
                       b.device->sm_er ? OWN_ADDRESS_RANDOM : OWN_ADDRESS_PUBLIC, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
    bls_ll_setAdvData(adv, n);
    bls_ll_setScanRspData((u8 *)p->device->scan_response, p->device->scan_response_len);
    bls_ll_setAdvEnable(BLC_ADV_ENABLE);
    b.advertising = 1;
    util_log("bt: advertising %s, %u bonds", p->name, blc_smp_param_getCurrentBondingDeviceNumber());
}

/* Wake is opt-in: only an explicit connect for an already bonded slot.
 * Clear the advertisement on the first connection, before automatic retries. */
void bt_connect(void)
{
    u8 adv[31], n;
    if (!b.device || b.device->sm_er || b.connected || b.stopping ||
        is_zero(b.paired.ltk, 16) || is_zero(b.paired.le_peer, 6)) return;
    b.wake = 1;
    n = b.device->advertising(&b.paired, 0, 1, adv);
    bls_ll_setAdvData(adv, n);
    util_log("bt: advertising to wake");
}

void bt_drop(void)
{
    if (b.connected && !b.stopping) bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
}

void bt_stop(void)
{
    if (!b.started) return;
    b.wake = 0;
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
    b.paired = *bond;
    n = b.device->advertising(bond, 0, 0, adv);
    bls_ll_setAdvData(adv, n);
}

/* No pairing or link set-up under way: a flash write is safe. */
int bt_radio_quiet(void)
{
    return !b.connected || b.encrypted || (b.device && !b.device->sm_er);
}

int bt_input_wanted(void)
{
    return b.connected && b.device && (b.encrypted || !b.device->sm_er) && plat_att_notifying(b.device->input_handle) && !plat_att_busy() &&
           blc_ll_getTxFifoNumber() < IN_FLIGHT;
}

/* The report without its id: on LE the report reference names it. */
int bt_send_input(u8 id, const u8 *data, u16 len)
{
    u32 fragments = (len + 7u + TX_OCTETS - 1) / TX_OCTETS;   /* with L2CAP and ATT headers */

    (void)id;
    if (!bt_input_wanted() || len > INPUT_MAX || blc_ll_getTxFifoNumber() + fragments > IN_FLIGHT) return 0;
    return plat_att_send(b.device->input_handle, data, len);
}

int bt_send_message(u16 handle, const u8 *data, u16 len)
{
    return b.connected && b.device && plat_att_message(handle, data, len);
}

int bt_poll(struct bt_ev *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (b.linked_pending) {
        b.linked_pending = 0;
        ev->kind = BT_EV_LINKED;
        memcpy(ev->bond.le_peer, b.paired.le_peer, 6);
        return 1;
    }
    if (b.paired_pending) {
        b.paired_pending = 0;
        ev->kind = BT_EV_PAIRED;
        ev->bond = b.paired;
        return 1;
    }
    if (b.write_pending) {
        b.write_pending = 0;
        ev->kind = BT_EV_WRITE;
        ev->write = b.write;
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
    u16 interval;
    if (!b.started) return 0;
    blc_sdk_main_loop();
    if (plat_ll_requested_interval(&interval)) util_log("bt: connect request interval %u", interval);
    if (b.connected) plat_att_poll();
    if (!b.connected && !b.advertising && b.device) {
        bls_ll_setAdvEnable(BLC_ADV_ENABLE);
        b.advertising = 1;
    }
    return b.linked_pending || b.unlinked_pending || b.paired_pending || b.stopped_pending || b.write_pending ? EV_BT : 0;
}
