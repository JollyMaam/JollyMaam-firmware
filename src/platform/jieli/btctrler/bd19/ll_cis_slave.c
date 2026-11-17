/* BD19 connected-isochronous slave lifecycle and HCI/LLCP commands.
 *
 * Public entry points are lctrCisStorePeerCisReq, le_cmd_set_cis_slave,
 * ll_cis_slave_send_rx_llcp_msg and ll_cis_slave_init.  The member has no
 * public globals.
 *
 * SDK quirks kept: CIS REQ storage copies the 52-byte peer request before
 * validating it, a masked LE CIS Request event is answered with reason 26,
 * and every CIS REQ path runs the accept helper after allocation/validation.
 */
#include "btctrler/glue.h"

#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_d_LL_CIS, log_tag_const_e_LL_CIS,
    log_tag_const_i_LL_CIS;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern void log_print(int level, const char *tag, const char *format, ...);
extern int printf(const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))
typedef u16 cis_word __attribute__((may_alias));
typedef u32 cis_dword __attribute__((may_alias));
#define CIS_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_cis_slave.c"

#ifdef CONFIG_CPU_BD19
extern u8 cis_slave_emit_meta_event(u8, const char *, ...) __asm__("__hci_emit_le_meta_event");
#else
extern bool __hci_emit_le_meta_event(u8, const char *, ...);
#endif

struct cis_ll_msg {
    u16 handle;
    u8 opcode;
    u8 reserved;
    u8 data[];
} __attribute__((aligned(4)));

static void ll_cis_slave_msg_handler(u8 msg_id, void *payload);
static void ll_cis_req_dump(const lctrCisReq_t *pCisReq);

static __attribute__((noinline)) void cpu_reset(void)
{
    p33_soft_reset();
}

static __inline__ __attribute__((always_inline)) void cis_assert_failed_policy(
    int ok, const char *what, int line, int assert_policy)
{
    if (ok)
        return;
    if (assert_policy) {
        printf("file:%s, line:%d", CIS_FILE, line);
        printf(what);
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
}

#define cis_assert_failed(ok, what, line) \
    cis_assert_failed_policy((ok), "ASSERT-FAILD: " what " ", (line), config_asser)

static __inline__ __attribute__((always_inline)) void msg_alloc_assert(
    void *message, int line)
{
    cis_assert_failed(message != NULL, "msg", line);
}

static __attribute__((noinline)) u8 lctrPhysBitToPhy(u8 phys_bit)
{
    return phys_bit == 4 ? 3 : (phys_bit == 2 ? 2 : 1);
}

static __inline__ __attribute__((always_inline)) u16 read_le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static __inline__ __attribute__((always_inline)) u32 read_le24(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

static __inline__ __attribute__((always_inline)) void write_le24(u8 *p, u32 value)
{
    *(cis_dword *)p = value;
}

void lctrCisStorePeerCisReq(le_cis_t *cis, lctrCisReq_t *cis_req) LL_CODE;
void lctrCisStorePeerCisReq(le_cis_t *cis, lctrCisReq_t *cis_req)
{
    __builtin_memcpy(&cis->data.slv.cisReq, cis_req, sizeof(*cis_req));
    cis->cisId = cis_req->cisId;
    cis->phyMToS = lctrPhysBitToPhy(cis_req->phyMToS);
    cis->phySToM = lctrPhysBitToPhy(cis_req->phySToM);
    cis->framing = cis_req->framing;
    cis->sduSizeMToS = cis_req->sduSizeMToS;
    cis->sduSizeSToM = cis_req->sduSizeSToM;
    cis->sduIntervalMToS = cis_req->sduIntervalMToS;
    cis->sduIntervalSToM = cis_req->sduIntervalSToM;
    cis->pduSizeMToS = cis_req->plMToS;
    cis->pduSizeSToM = cis_req->plSToM;
    cis->nse = cis_req->nse;
    cis->subIntervUsec = cis_req->subIntervUsec;
    cis->bnMToS = cis_req->bnMToS;
    cis->bnSToM = cis_req->bnSToM;
    cis->ftMToS = cis_req->ftMToS;
    cis->ftSToM = cis_req->ftSToM;
    cis->isoInterval = cis_req->isoInterval;
    cis->delayUsec = cis_req->subIntervUsec;
    ll_cis_req_dump(cis_req);
}

static void ll_cis_req_dump(const lctrCisReq_t *pCisReq)
    __attribute__((noinline)) LL_CODE;
static void ll_cis_req_dump(const lctrCisReq_t *pCisReq)
{
    if (!log_tag_const_i_LL_CIS)
        return;
    log_print(2, NULL, "[Info]: [LL_CIS]---------------------------\r\n");
    log_print(2, NULL, "[Info]: [LL_CIS]cigId 0x%x\r\n", pCisReq->cigId);
    log_print(2, NULL, "[Info]: [LL_CIS]cisId 0x%x\r\n", pCisReq->cisId);
    log_print(2, NULL, "[Info]: [LL_CIS]phyMToS 0x%x\r\n", pCisReq->phyMToS);
    log_print(2, NULL, "[Info]: [LL_CIS]phySToM 0x%x\r\n", pCisReq->phySToM);
    log_print(2, NULL, "[Info]: [LL_CIS]framing 0x%x\r\n", pCisReq->framing);
    log_print(2, NULL, "[Info]: [LL_CIS]sduSizeMToS 0x%x\r\n",
              pCisReq->sduSizeMToS);
    log_print(2, NULL, "[Info]: [LL_CIS]sduSizeSToM 0x%x\r\n",
              pCisReq->sduSizeSToM);
    log_print(2, NULL, "[Info]: [LL_CIS]sduIntervalMToS 0x%x\r\n",
              pCisReq->sduIntervalMToS);
    log_print(2, NULL, "[Info]: [LL_CIS]sduIntervalSToM 0x%x\r\n",
              pCisReq->sduIntervalSToM);
    log_print(2, NULL, "[Info]: [LL_CIS]plMToS 0x%x\r\n", pCisReq->plMToS);
    log_print(2, NULL, "[Info]: [LL_CIS]plSToM 0x%x\r\n", pCisReq->plSToM);
    log_print(2, NULL, "[Info]: [LL_CIS]nse 0x%x\r\n", pCisReq->nse);
    log_print(2, NULL, "[Info]: [LL_CIS]subIntervUsec 0x%x\r\n",
              pCisReq->subIntervUsec);
    log_print(2, NULL, "[Info]: [LL_CIS]bnMToS 0x%x\r\n", pCisReq->bnMToS);
    log_print(2, NULL, "[Info]: [LL_CIS]bnSToM 0x%x\r\n", pCisReq->bnSToM);
    log_print(2, NULL, "[Info]: [LL_CIS]ftMToS 0x%x\r\n", pCisReq->ftMToS);
    log_print(2, NULL, "[Info]: [LL_CIS]ftSToM 0x%x\r\n", pCisReq->ftSToM);
    log_print(2, NULL, "[Info]: [LL_CIS]isoInterval 0x%x\r\n",
              pCisReq->isoInterval);
    log_print(2, NULL, "[Info]: [LL_CIS]cisOffMinUsec 0x%x\r\n",
              pCisReq->cisOffMinUsec);
    log_print(2, NULL, "[Info]: [LL_CIS]cisOffMaxUsec 0x%x\r\n",
              pCisReq->cisOffMaxUsec);
    log_print(2, NULL, "[Info]: [LL_CIS]ceRef 0x%x\r\n", pCisReq->ceRef);
    log_print(2, NULL, "[Info]: [LL_CIS]---------------------------\r\n");
}

static u8 ll_accept_cis_req(u16 cis_handle) LL_CODE;
static u8 ll_accept_cis_req(u16 cis_handle)
{
    le_cis_t *cis = ll_cis_by_handle(cis_handle);
    if (!cis) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]LctrAcceptCisReq, invalid CIS handle\r\n");
        return 2;
    }
    struct le_link *link = ll_link_for_handle(cis->aclHandle);
    if (!link) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]LctrAcceptCisReq, invalid ACL handle\r\n");
        return 2;
    }
    if (link->role != LL_ROLE_SLAVE) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]LctrAcceptCisReq, invalid role\r\n");
        return 12;
    }

    u8 *message = ll_iso_msg_alloc(2);
    msg_alloc_assert(message, 350);
    *(cis_word *)message = 14;
    *(cis_word *)(message + 2) = LL_CIS_SLV_MSG_ACCEPT_CIS;
    *(cis_word *)(message + 4) = link->cisHandle;
    ll_iso_msg_send(message);
    return 0;
}

static u8 ll_reject_cis_req(u16 cis_handle, u8 reason) LL_CODE;
static u8 ll_reject_cis_req(u16 cis_handle, u8 reason)
{
    le_cis_t *cis = ll_cis_by_handle(cis_handle);
    if (!cis) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]invalid CIS handle\r\n");
        return 2;
    }
    struct le_link *link = ll_link_for_handle(cis->aclHandle);
    if (!link) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]invalid ACL handle\r\n");
        return 2;
    }
    if (link->role != LL_ROLE_SLAVE) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]invalid role\r\n");
        return 12;
    }

    u8 *message = ll_iso_msg_alloc(4);
    msg_alloc_assert(message, 401);
    *(cis_word *)message = 14;
    *(cis_word *)(message + 2) = LL_CIS_SLV_MSG_REJECT_CIS;
    *(cis_word *)(message + 4) = link->cisHandle;
    *(u8 *)(message + 6) = reason;
    ll_iso_msg_send(message);
    return 0;
}

static u8 LctrCheckCisReq(struct le_link *link,
                          const lctrCisReq_t *pCisReq) LL_CODE;
static u8 LctrCheckCisReq(struct le_link *link, const lctrCisReq_t *pCisReq)
{
    if ((u32)(pCisReq->isoInterval - 4) > 3196 ||
        pCisReq->nse > 31 ||
        pCisReq->framing > 1 ||
        pCisReq->sduSizeMToS > 4095 ||
        pCisReq->sduSizeSToM > 4095 ||
        pCisReq->sduIntervalMToS > 1048575 ||
        pCisReq->sduIntervalSToM > 1048575 ||
        pCisReq->plMToS > 4091 ||
        pCisReq->plSToM > 4091 ||
        pCisReq->phyMToS > 4 ||
        pCisReq->phySToM > 4 ||
        (u8)(pCisReq->ftMToS - 1) > 30 ||
        (u8)(pCisReq->ftSToM - 1) > 30 ||
        pCisReq->bnMToS > 16 ||
        pCisReq->bnSToM > 16) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]LctrCheckCisReq: invalid parameters, parameter out of range\r\n");
        return 0;
    }
    if (pCisReq->cisOffMaxUsec < pCisReq->cisOffMinUsec) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]LctrCheckCisReq: invalid parameters, cisOffMaxUsec shall be greater than cisOffMinUsec\r\n");
        return 0;
    }
    u32 max_offset = (u32)link->ll_data.interval * 1250 -
                     pCisReq->subIntervUsec * pCisReq->nse +
                     ll_iso_hdl.cisSubEvtSpaceDelay;
    if (pCisReq->cisOffMaxUsec > max_offset) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]LctrCheckCisReq: invalid parameters, cisOffMaxUsec is too big\r\n");
        return 0;
    }
    return 1;
}

static void llcp_slv_recv_cis_req(struct le_link *link,
                                  const lctrCisReq_t *cis_req) LL_CODE;
static void llcp_slv_recv_cis_req(struct le_link *link,
                                  const lctrCisReq_t *cis_req)
{
    le_cig_t *cig = ll_cig_by_id(cis_req->cigId);
    if (!cig) {
        cig = ll_cig_alloc(cis_req->cigId);
        if (!cig) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL,
                          "<Error>: [LL_CIS]Not able to allocate a CIG context\r\n");
            ll_send_control_data(link, LL_REJECT_IND_EXT, "11", LL_PDU_CIS_REQ, 67);
            ll_accept_cis_req(link->cisHandle);
            return;
        }
    }

    le_cis_t *cis = ll_cis_alloc(cig);
    if (!cis) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]Not able to allocate a CIS context\r\n");
        ll_send_control_data(link, LL_REJECT_IND_EXT, "11", LL_PDU_CIS_REQ, 67);
    }

    link->cisHandle = cis->cisHandle;
    cis->aclHandle = link->handle;
    cis->role = LL_ROLE_SLAVE;
    ll_cis_req_dump(cis_req);

    if (!LctrCheckCisReq(link, cis_req)) {
        ll_send_control_data(link, LL_REJECT_IND_EXT, "11", LL_PDU_CIS_REQ, 30);
        ll_cis_free(cis);
    } else if ((config_btctler_le_features & ((u64)1 << 32)) == 0) {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]Not Support ISO Feature\r\n");
        ll_send_control_data(link, LL_REJECT_IND_EXT, "11", LL_PDU_CIS_REQ, 26);
        ll_cis_free(cis);
    } else {
        lctrCisStorePeerCisReq(cis, (lctrCisReq_t *)cis_req);
#ifdef CONFIG_CPU_BD19
        u8 ret = cis_slave_emit_meta_event(
            26, "HH11", cis->aclHandle, cis->cisHandle, cig->cigId, cis->cisId);
#else
        u8 ret = __hci_emit_le_meta_event(
            26, "HH11", cis->aclHandle, cis->cisHandle, cig->cigId, cis->cisId);
#endif
        if (ret == 0) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL,
                          "<Error>: [LL_CIS]Host event masked; reply with reason=UNSUPPORTED_REMOTE_FEATURE, cisHandle=%u\r\n",
                          cis->cisHandle);
            ll_reject_cis_req(cis->cisHandle, 26);
        }
    }
    ll_accept_cis_req(link->cisHandle);
}

static u8 llcp_slv_recv_cis_ind(struct le_link *link,
                                const lctrCisInd_t *cis_ind) LL_CODE;
static u8 llcp_slv_recv_cis_ind(struct le_link *link,
                                const lctrCisInd_t *cis_ind)
{
    le_cis_t *cis = ll_cis_by_handle(link->cisHandle);
    int assert_policy = config_asser;
    cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 551, assert_policy);
    cis->accessAddr = cis_ind->accessAddr;
    cis->offsetUsec = cis_ind->cisOffUsec;
    cis->cigSyncDelayUsec = cis_ind->cigSyncDelayUsec;
    cis->cisSyncDelayUsec = cis_ind->cisSyncDelayUsec;
    cis->ceRef = cis_ind->ceRef;
    if (log_tag_const_i_LL_CIS) {
        log_print(2, NULL, "[Info]: [LL_CIS]accessAddr       0x%x\r\n",
                  cis->accessAddr);
        log_print(2, NULL, "[Info]: [LL_CIS]offsetUsec       0x%x\r\n",
                  cis->offsetUsec);
        log_print(2, NULL, "[Info]: [LL_CIS]cigSyncDelayUsec 0x%x\r\n",
                  cis->cigSyncDelayUsec);
        log_print(2, NULL, "[Info]: [LL_CIS]cisSyncDelayUsec 0x%x\r\n",
                  cis->cisSyncDelayUsec);
        log_print(2, NULL, "[Info]: [LL_CIS]ceRef            0x%x\r\n",
                  cis->ceRef);
    }
    __builtin_memcpy(&cis->crcInit, link->ll_data.crcinit, 3);
    cis->supTimeoutMs = link->ll_data.timeout;

    le_cig_t *cig = cis->cig;
    cis_assert_failed_policy(cig != NULL, "ASSERT-FAILD: cig ", 574, assert_policy);
    if (!cig->isBodBuilt) {
        cig->cigSyncDelayUsec = cis->cigSyncDelayUsec;
        cig->isoInterval = cis->isoInterval;
        ll_cig_add_cis(cig, cis);
    } else {
        ll_cig_add_cis_tail(cig, cis);
    }

    s32 anchor_delta = (s32)cis->ceRef - link->event_cnt;
    if (anchor_delta > 32767) {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]first anchor in the past\r\n");
    } else {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]first anchor in the future\r\n");
        cis->data.slv.anchorOffsetUsec =
            (u32)link->ll_data.interval * 1250 *
                ((s32)cis->ceRef - link->event_cnt) + cis->offsetUsec;
    }

    if (!cig->isBodStarted) {
        cig->packing = 0;
        cis->cisCeRef = 0;
    } else {
        u32 cig_end = (u32)link->ll_data.interval * 1250 *
                      ((s32)cis->ceRef + 1 - link->event_cnt);
        cis->cisCeRef = 0;
        u32 anchor = cig->roleData.slv.anchorPointUsec;
        u32 has_event = cig->roleData.slv.cigEvtCounter != 0;
        u32 interval = (u32)cig->isoInterval * 1250;
        u16 cis_count = 0;
        do {
            cis_count++;
        } while (interval * ((u32)cis_count + has_event) + anchor <= cig_end);
        cis->cisCeRef = cis_count;
    }

    u8 *message = ll_iso_msg_alloc(2);
    cis_assert_failed_policy(message != NULL, "ASSERT-FAILD: msg ", 701, assert_policy);
    *(cis_word *)message = 12;
    *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_SUCCESS;
    *(cis_word *)(message + 4) = link->cisHandle;
    ll_iso_msg_send(message);
    return 0;
}

void ll_cis_slave_send_rx_llcp_msg(struct le_link *link,
                                   struct ble_rx *rx) LL_CODE;
void ll_cis_slave_send_rx_llcp_msg(struct le_link *link, struct ble_rx *rx)
{
    u8 opcode = rx->data[0];
    if (opcode != LL_PDU_CIS_REQ && opcode != LL_PDU_CIS_IND)
        return;

    int assert_policy = config_asser;
    u8 *message = ll_iso_msg_alloc(56);
    cis_assert_failed_policy(message != NULL, "ASSERT-FAILD: msg ", 856, assert_policy);
    opcode = rx->data[0];
    *(cis_word *)message = 14;
    *(cis_word *)(message + 2) = LL_CIS_SLV_MSG_RX_LLCP;
    *(u8 *)(message + 6) = opcode;
    *(cis_word *)(message + 4) = link->handle;

    u8 *payload = message + 8;
    if (opcode == LL_PDU_CIS_REQ) {
        const u8 *p = rx->data + 1;
        payload[0] = p[0];
        payload[1] = p[1];
        payload[2] = p[2];
        payload[3] = p[3];
        payload[4] = p[5] >> 7;
        *(cis_word *)(payload + 6) = (u16)(p[4] | ((p[5] << 8) & 0x0f00));
        *(cis_word *)(payload + 8) = (u16)(p[6] | ((p[7] << 8) & 0x0f00));
        write_le24(payload + 12, read_le24(p + 8));
        write_le24(payload + 16, read_le24(p + 11));
        *(cis_word *)(payload + 20) = read_le16(p + 14);
        *(cis_word *)(payload + 22) = read_le16(p + 16);
        payload[24] = p[18];
        write_le24(payload + 28, read_le24(p + 19));
        payload[33] = p[22] >> 4;
        payload[32] = p[22] & 0x0f;
        payload[34] = p[23];
        payload[35] = p[24];
        *(cis_word *)(payload + 36) = read_le16(p + 25);
        write_le24(payload + 40, read_le24(rx->data + 28));
        write_le24(payload + 44, read_le24(rx->data + 31));
        *(cis_word *)(payload + 48) = read_le16(rx->data + 34);
    } else if (opcode == LL_PDU_CIS_IND) {
        *(cis_dword *)(payload + 0) = (u32)rx->data[1] |
                                ((u32)rx->data[2] << 8) |
                                ((u32)rx->data[3] << 16) |
                                ((u32)rx->data[4] << 24);
        write_le24(payload + 4, read_le24(rx->data + 5));
        write_le24(payload + 8, read_le24(rx->data + 8));
        write_le24(payload + 12, read_le24(rx->data + 11));
        *(cis_word *)(payload + 16) = read_le16(rx->data + 14);
    }
    ll_iso_msg_send(message);
}

void ll_cis_slave_init(void) LL_CODE;
void ll_cis_slave_init(void)
{
    ll_dispatch_msg_table[14] = ll_cis_slave_msg_handler;
}

static void ll_cis_slave_msg_handler(u8 msg_id, void *payload) LL_CODE;
static void ll_cis_slave_msg_handler(u8 msg_id, void *payload)
{
    switch (msg_id) {
    case LL_CIS_SLV_MSG_ACCEPT_CIS: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL,
                      "[Info]: [LL_CIS]--> LL_CIS_SLV_MSG_ACCEPT_CIS\r\n");
        u16 cis_handle = *(cis_word *)payload;
        le_cis_t *cis = ll_cis_by_handle(cis_handle);
        cis_assert_failed(cis != NULL, "cis", 253);
        struct le_link *link = ll_link_for_handle(cis->aclHandle);
        ll_response_timeout_start(link);
        u32 cis_off_min = cis->data.slv.cisReq.cisOffMinUsec;
        u32 cis_off_max = cis->data.slv.cisReq.cisOffMaxUsec;
        u16 ce_ref = cis->data.slv.cisReq.ceRef;
        if (cis_off_min > cis_off_max) {
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]Proposed offset cannot be scheduled within cisOffMaxUsec=%d\r\n",
                          cis_off_max);
            ll_send_control_data(link, LL_REJECT_IND_EXT, "11",
                                 LL_PDU_CIS_REQ, 17);
        } else {
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]<- LL_PDU_CIS_RSP\r\n");
            ll_send_control_data(link, LL_PDU_CIS_RSP, "332",
                                 cis_off_min, cis_off_max, ce_ref);
        }
        break;
    }
    case LL_CIS_SLV_MSG_REJECT_CIS: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL,
                      "[Info]: [LL_CIS]--> LL_CIS_SLV_MSG_REJECT_CIS\r\n");
        u16 cis_handle = *(cis_word *)payload;
        u8 reason = *(u8 *)((u8 *)payload + 2);
        le_cis_t *cis = ll_cis_by_handle(cis_handle);
        struct le_link *link = ll_link_for_handle(cis->aclHandle);
        ll_send_control_data(link, LL_REJECT_IND_EXT, "11",
                             LL_PDU_CIS_REQ, reason);
        ll_cis_destory(cis);
        break;
    }
    case LL_CIS_SLV_MSG_RX_LLCP: {
        struct cis_ll_msg *data = payload;
        struct le_link *link = ll_link_for_handle(data->handle);
        le_cis_t *cis = ll_cis_by_handle(link->cisHandle);
        (void)cis;
        switch (data->opcode) {
        case LL_PDU_CIS_REQ:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_PDU_CIS_REQ\r\n");
            llcp_slv_recv_cis_req(link, (lctrCisReq_t *)data->data);
            break;
        case LL_PDU_CIS_IND:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_PDU_CIS_IND\r\n");
            llcp_slv_recv_cis_ind(link, (lctrCisInd_t *)data->data);
            break;
        case LL_UNKNOWN_RSP:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_UNKNOWN_RSP\r\n");
            if (data->data[0] == LL_PDU_CIS_REQ) {
                u8 *message = ll_iso_msg_alloc(4);
                msg_alloc_assert(message, 789);
                *(cis_word *)message = 12;
                *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_FAIL;
                *(cis_word *)(message + 4) = link->cisHandle;
                *(u8 *)(message + 6) = 26;
                ll_iso_msg_send(message);
            }
            break;
        }
        break;
    }
    }
}

int le_cmd_set_cis_slave(u16 opcode, const u8 *data, u32 size) LL_CODE;
int le_cmd_set_cis_slave(u16 opcode, const u8 *data, u32 size)
{
    (void)size;
    u16 ocf = opcode & 0x3ff;
    switch (ocf) {
    case 0x66: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> HCI_LE_ACCEPT_CIS_REQ\r\n");
        u16 cis_handle = btctler_little_endian_read_16((u8 *)data, 0);
        u8 status = ll_accept_cis_req(cis_handle);
        hci_send_event(15, "112", status, 1, opcode);
        return 1;
    }
    case 0x67: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> HCI_LE_REJECT_CIS_REQ\r\n");
        u16 cis_handle = btctler_little_endian_read_16((u8 *)data, 0);
        u8 reason = data[2];
        u8 status = ll_reject_cis_req(cis_handle, reason);
        hci_send_event_cmd_complete(opcode, "1H", status, cis_handle);
        return 1;
    }
    default:
        return 0;
    }
}

#ifdef UNITTEST
#include "test.h"

#include <stdarg.h>

ll_dispatch_msg_t ll_dispatch_msg_table[15];
u8 log_tag_const_d_LL_CIS, log_tag_const_e_LL_CIS, log_tag_const_i_LL_CIS;
u64 config_btctler_le_features;
int config_asser;

static u8 test_message[64];
static int test_alloc_calls;
static int test_send_calls;
static u16 test_alloc_size;

ll_iso_handle_t ll_iso_hdl;
static le_cis_t fixture_cis;
static le_cig_t fixture_cig;
static struct le_link fixture_link;
static int cis_alive, no_cig, cig_alloc_fail, cis_lookups, freed_cis;
static int head_adds, tail_adds, timeout_starts, timeout_mutate;
static int control_opcode, control_args[20], control_calls;
static int meta_result = 1, meta_calls, meta_args[4];
static int hci_status, hci_opcode;
static u8 queued[8][64];
static struct ble_rx *mutate_rx;
static int mutate_anchor_log;

u16 btctler_little_endian_read_16(const u8 *p, int offset) { return read_le16(p + offset); }
struct le_link *ll_link_for_handle(int handle) { return handle == fixture_link.handle ? &fixture_link : NULL; }
le_cis_t *ll_cis_by_handle(u16 handle)
{
    cis_lookups++;
    return cis_alive && handle == fixture_cis.cisHandle ? &fixture_cis : NULL;
}
le_cig_t *ll_cig_by_id(u8 id) { (void)id; return no_cig ? NULL : &fixture_cig; }
le_cig_t *ll_cig_alloc(u8 id) { fixture_cig.cigId = id; return cig_alloc_fail ? NULL : &fixture_cig; }
le_cis_t *ll_cis_alloc(le_cig_t *cig)
{
    memset(&fixture_cis, 0, sizeof(fixture_cis));
    fixture_cis.cisHandle = 0x1234; fixture_cis.cig = cig; cis_alive = 1;
    return &fixture_cis;
}
void ll_cis_free(le_cis_t *cis) { CHECK(cis == &fixture_cis); cis_alive = 0; freed_cis++; }
void ll_cis_destory(le_cis_t *cis) { ll_cis_free(cis); }
void ll_cig_add_cis(le_cig_t *cig, le_cis_t *cis) { CHECK(cig == &fixture_cig && cis == &fixture_cis); head_adds++; }
void ll_cig_add_cis_tail(le_cig_t *cig, le_cis_t *cis) { CHECK(cig == &fixture_cig && cis == &fixture_cis); tail_adds++; }
void ll_response_timeout_start(struct le_link *link)
{
    CHECK(link == &fixture_link); timeout_starts++;
    if (timeout_mutate) fixture_cis.data.slv.cisReq.cisOffMaxUsec = 0;
}
void ll_send_control_data(struct le_link *link, int opcode, const char *format, ...)
{
    CHECK(link == &fixture_link); control_calls++; control_opcode = opcode;
    va_list ap; va_start(ap, format);
    for (size_t i = 0; i < strlen(format); i++) control_args[i] = va_arg(ap, int);
    va_end(ap);
}
u8 cis_slave_emit_meta_event(u8 event, const char *format, ...)
{
    CHECK(event == 26 && !strcmp(format, "HH11")); meta_calls++;
    va_list ap; va_start(ap, format);
    for (int i = 0; i < 4; i++) meta_args[i] = va_arg(ap, int);
    va_end(ap); return meta_result;
}
int hci_send_event(int event, const char *format, ...)
{
    CHECK(event == 15 && !strcmp(format, "112")); va_list ap; va_start(ap, format);
    hci_status = va_arg(ap, int); CHECK(va_arg(ap, int) == 1); hci_opcode = va_arg(ap, int); va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    CHECK(!strcmp(format, "1H")); hci_opcode = opcode; va_list ap; va_start(ap, format);
    hci_status = va_arg(ap, int); (void)va_arg(ap, int); va_end(ap); return 0;
}
static void reset_fixture(void)
{
    memset(&fixture_link, 0, sizeof(fixture_link)); memset(&fixture_cig, 0, sizeof(fixture_cig));
    memset(&fixture_cis, 0, sizeof(fixture_cis));
    fixture_link.handle = 0x242; fixture_link.cisHandle = 0x1234; fixture_link.role = LL_ROLE_SLAVE;
    fixture_link.ll_data.interval = 100; fixture_link.event_cnt = 10;
    fixture_link.ll_data.timeout = 1000; fixture_link.ll_data.crcinit[0] = 0x12;
    fixture_cis.cisHandle = 0x1234; fixture_cis.aclHandle = fixture_link.handle;
    fixture_cis.cig = &fixture_cig; fixture_cis.isoInterval = 4; cis_alive = 1;
    no_cig = cig_alloc_fail = freed_cis = meta_calls = 0; meta_result = 1;
    test_send_calls = test_alloc_calls = cis_lookups = control_calls = timeout_starts = 0;
    head_adds = tail_adds = timeout_mutate = 0;
    config_btctler_le_features = (u64)1 << 32;
}

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
}

void *ll_iso_msg_alloc(u16 size)
{
    test_alloc_calls++;
    if (mutate_rx) { *((u8 *)mutate_rx + offsetof(struct ble_rx, data)) = 0xff; mutate_rx = NULL; }
    test_alloc_size = size;
    memset(test_message, 0xaa, sizeof(test_message));
    return test_message;
}

void ll_iso_msg_send(void *message)
{
    CHECK(message == test_message);
    CHECK(test_send_calls < 8);
    memcpy(queued[test_send_calls++], message, sizeof(test_message));
}

void p33_soft_reset(void)
{
    CHECK(0);
}

void cpu_assert_debug(void)
{
    CHECK(0);
}

int main(void)
{
    lctrCisReq_t request;
    memset(&request, 0, sizeof(request));
    request.cigId = 7;
    request.cisId = 3;
    request.phyMToS = 2;
    request.phySToM = 4;
    request.framing = LL_ISO_PDU_TYPE_FRAMED;
    request.sduSizeMToS = 872;
    request.sduSizeSToM = 900;
    request.sduIntervalMToS = 5000;
    request.sduIntervalSToM = 5000;
    request.plMToS = 800;
    request.plSToM = 820;
    request.nse = 4;
    request.subIntervUsec = 1250;
    request.bnMToS = 2;
    request.bnSToM = 2;
    request.ftMToS = 4;
    request.ftSToM = 4;
    request.isoInterval = 4;
    request.cisOffMinUsec = 0x1234;
    request.cisOffMaxUsec = 0x2345;
    request.ceRef = 0x5678;

    le_cis_t cis;
    memset(&cis, 0, sizeof(cis));
    lctrCisStorePeerCisReq(&cis, &request);
    CHECK(cis.cisId == 3 && cis.framing == LL_ISO_PDU_TYPE_FRAMED);
    CHECK(cis.phyMToS == 2 && cis.phySToM == 3);
    CHECK(cis.sduSizeMToS == 872 && cis.sduSizeSToM == 900);
    CHECK(cis.sduIntervalMToS == 5000 && cis.sduIntervalSToM == 5000);
    CHECK(cis.pduSizeMToS == 800 && cis.pduSizeSToM == 820);
    CHECK(cis.nse == 4 && cis.subIntervUsec == 1250);
    CHECK(cis.bnMToS == 2 && cis.bnSToM == 2 && cis.delayUsec == 1250);
    CHECK(!memcmp(&cis.data.slv.cisReq, &request, sizeof(request)));

    union {
        struct ble_rx rx;
        u8 bytes[sizeof(struct ble_rx) + 40];
    } packet;
    memset(&packet, 0, sizeof(packet));
    u8 *packet_data = packet.bytes + offsetof(struct ble_rx, data);
    struct le_link link;
    memset(&link, 0, sizeof(link));
    link.handle = 0x0242;

    packet_data[0] = LL_PDU_CIS_REQ;
    packet_data[1] = 7;
    packet_data[2] = 3;
    packet_data[3] = 2;
    packet_data[4] = 4;
    packet_data[5] = 0x68;
    packet_data[6] = 0x83;
    packet_data[7] = 0x84;
    packet_data[8] = 0x03;
    packet_data[9] = 0x88;
    packet_data[10] = 0x13;
    packet_data[11] = 0;
    packet_data[12] = 0x88;
    packet_data[13] = 0x13;
    packet_data[14] = 0;
    packet_data[15] = 0x20;
    packet_data[16] = 0x03;
    packet_data[17] = 0x34;
    packet_data[18] = 0x03;
    packet_data[19] = 4;
    packet_data[20] = 0xe2;
    packet_data[21] = 0x04;
    packet_data[22] = 0;
    packet_data[23] = 0x22;
    packet_data[24] = 4;
    packet_data[25] = 4;
    packet_data[26] = 4;
    packet_data[27] = 0;
    packet_data[28] = 0x34;
    packet_data[29] = 0x12;
    packet_data[30] = 0;
    packet_data[31] = 0x45;
    packet_data[32] = 0x23;
    packet_data[33] = 0;
    packet_data[34] = 0x78;
    packet_data[35] = 0x56;

    ll_cis_slave_send_rx_llcp_msg(&link, &packet.rx);
    CHECK(test_alloc_calls == 1 && test_alloc_size == 56 && test_send_calls == 1);
    CHECK(*(cis_word *)(test_message + 0) == 14);
    CHECK(*(cis_word *)(test_message + 2) == LL_CIS_SLV_MSG_RX_LLCP);
    CHECK(*(cis_word *)(test_message + 4) == link.handle);
    CHECK(test_message[6] == LL_PDU_CIS_REQ);
    CHECK(test_message[8] == 7 && test_message[9] == 3);
    CHECK(test_message[10] == 2 && test_message[11] == 4);
    CHECK(test_message[12] == LL_ISO_PDU_TYPE_FRAMED);
    CHECK(*(cis_word *)(test_message + 14) == (((0x83 << 8) & 0x0f00) | 0x68));
    CHECK(*(cis_word *)(test_message + 16) == (((0x03 << 8) & 0x0f00) | 0x84));
    CHECK(*(cis_dword *)(test_message + 20) == 0x001388);
    CHECK(*(cis_dword *)(test_message + 24) == 0x001388);
    CHECK(*(cis_dword *)(test_message + 48) == 0x1234);
    CHECK(*(cis_dword *)(test_message + 52) == 0x2345);
    CHECK(*(cis_word *)(test_message + 56) == 0x5678);

    memset(ll_dispatch_msg_table, 0, sizeof(ll_dispatch_msg_table));
    ll_cis_slave_init();
    CHECK(ll_dispatch_msg_table[14] == ll_cis_slave_msg_handler);
    /* Exercise the registered handler, including the SDK common accept after
     * reject/free or CIG allocation failure. Raw IR branches 82/203/230->233. */
    u32 storage[16] = {0};
    struct cis_ll_msg *incoming = (void *)storage;
    reset_fixture();
    incoming->handle = fixture_link.handle; incoming->opcode = LL_PDU_CIS_REQ;
    request.cisOffMinUsec = 10; request.cisOffMaxUsec = 20;
    memcpy(incoming->data, &request, sizeof(request));
    meta_result = 0;
    ll_cis_slave_msg_handler(LL_CIS_SLV_MSG_RX_LLCP, incoming);
    CHECK(meta_calls == 1 && meta_args[0] == 0x242 && meta_args[1] == 0x1234);
    CHECK(test_send_calls == 2 && *(cis_word *)queued[0] == 14 && *(cis_word *)(queued[0]+2) == 1);
    CHECK(queued[0][6] == 26 && *(cis_word *)(queued[1]+2) == 0);
    CHECK(!memcmp(&fixture_cis.data.slv.cisReq, &request, sizeof(request)));

    reset_fixture(); no_cig = cig_alloc_fail = 1;
    ll_cis_slave_msg_handler(LL_CIS_SLV_MSG_RX_LLCP, incoming);
    CHECK(control_opcode == LL_REJECT_IND_EXT && control_args[0] == LL_PDU_CIS_REQ && control_args[1] == 67);
    CHECK(test_send_calls == 1 && *(cis_word *)(queued[0]+2) == 0); /* common accept still ran */

    reset_fixture(); ((lctrCisReq_t *)incoming->data)->isoInterval = 3;
    ll_cis_slave_msg_handler(LL_CIS_SLV_MSG_RX_LLCP, incoming);
    CHECK(control_opcode == LL_REJECT_IND_EXT && control_args[1] == 30);
    CHECK(freed_cis == 1 && test_send_calls == 0 && cis_lookups >= 2);
    memcpy(incoming->data, &request, sizeof(request));

    reset_fixture(); config_btctler_le_features = 0;
    ll_cis_slave_msg_handler(LL_CIS_SLV_MSG_RX_LLCP, incoming);
    CHECK(control_opcode == LL_REJECT_IND_EXT && control_args[1] == 26 && freed_cis == 1);

    reset_fixture(); fixture_cis.data.slv.cisReq.cisOffMinUsec = 10;
    fixture_cis.data.slv.cisReq.cisOffMaxUsec = 20; timeout_mutate = 1;
    u32 handles = fixture_cis.cisHandle;
    ll_cis_slave_msg_handler(LL_CIS_SLV_MSG_ACCEPT_CIS, &handles);
    CHECK(timeout_starts == 1 && control_opcode == LL_REJECT_IND_EXT && control_args[1] == 17);

    /* A 16-bit event counter requires >255 iterations. Deliberately choose
     * distinct anchorPointUsec and cigEvtCounter so field confusion cannot pass. */
    reset_fixture(); fixture_cig.isBodBuilt = fixture_cig.isBodStarted = 1;
    fixture_cig.isoInterval = 4; fixture_cig.roleData.slv.anchorPointUsec = 70000;
    fixture_cig.roleData.slv.cigEvtCounter = 1; fixture_link.ll_data.interval = 4;
    lctrCisInd_t indication = {.accessAddr=0x12345678, .cisOffUsec=123, .cigSyncDelayUsec=456,
                             .cisSyncDelayUsec=234, .ceRef=400};
    CHECK(llcp_slv_recv_cis_ind(&fixture_link, &indication) == 0);
    /* RHS=4*1250*(400+1-10)=1955000; first count with 5000*(count+1)+70000 > RHS is 377. */
    CHECK(fixture_cis.cisCeRef == 377 && tail_adds == 1);
    CHECK(test_send_calls == 1 && *(cis_word *)queued[0] == 12 && *(cis_word *)(queued[0]+2) == 1);
    reset_fixture(); fixture_cig.isBodBuilt = 1; fixture_cig.packing = 1;
    llcp_slv_recv_cis_ind(&fixture_link, &indication);
    CHECK(fixture_cig.packing == 0 && fixture_cig.isBodBuilt == 1 && fixture_cis.cisCeRef == 0);

    /* Opcode is reloaded after allocation; unsupported opcodes queue metadata
     * with untouched payload, while REQ->IND switches unpacking paths. */
    reset_fixture(); packet_data[0] = LL_PDU_CIS_REQ; mutate_rx = &packet.rx;
    ll_cis_slave_send_rx_llcp_msg(&fixture_link, &packet.rx);
    CHECK(test_message[6] == 0xff && test_message[8] == 0xaa);

    reset_fixture(); u8 hci[3] = {0x34,0x12,0x24};
    CHECK(le_cmd_set_cis_slave(0x2066, hci, 0) == 1 && hci_opcode == 0x2066 && hci_status == 0);
    CHECK(le_cmd_set_cis_slave(0x2067, hci, 0) == 1 && hci_status == 0 && test_message[6] == 0x24);
    hci[0] = 0; CHECK(le_cmd_set_cis_slave(0x2066, hci, 0) == 1 && hci_status == 2);

    return TEST_DONE("btctrler_ll_cis_slave");
}
#endif
