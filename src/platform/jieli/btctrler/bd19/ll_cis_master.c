/* BD19 connected-isochronous master lifecycle and HCI/LLCP commands.
 *
 * The command dispatcher keeps the SDK's distinct production and test
 * parameter layouts, validation, allocation and packing.  Public entry points
 * are le_cmd_set_cis_master, ll_cis_master_send_rx_llcp_msg and
 * ll_cis_master_init; lctrMstCreateCisPend is the member's only moved global.
 *
 * SDK quirks kept: an empty CIG is queued with its first pair read just past
 * the command data, ll_create_cis marks the global pending flag before posting
 * its internal message, and a zero-CIS create completes without allocating.
 */
#include "btctrler/glue.h"

#include <math.h>
/* The SDK ceil declaration is readnone/nounwind. */
extern double ceil(double) __attribute__((const, nothrow));
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_d_LL_CIS, log_tag_const_e_LL_CIS,
    log_tag_const_i_LL_CIS;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern void log_print(int level, const char *tag, const char *format, ...);
extern int printf(const char *format, ...);
extern int hci_send_event_cmd_complete_payload(int opcode, u8 *payload, u8 size);

#define LL_CODE __attribute__((section(".ble_ll_code")))
typedef u16 cis_word __attribute__((may_alias));
typedef u32 cis_dword __attribute__((may_alias));
#define CIS_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_cis_master.c"

#ifdef CONFIG_CPU_BD19
/* The BD19 SDK event emitter returns a byte; the recovered BR23 prototype
 * returns bool. */
extern u8 cis_master_emit_meta_event(u8, const char *, ...) __asm__("__hci_emit_le_meta_event");
#else
extern bool __hci_emit_le_meta_event(u8, const char *, ...);
#endif

/* Dispatch passes a pointer to the ll_msg_t payload. */
struct cis_ll_msg {
    u16 handle;
    u8 opcode;
    u8 reserved;
    u8 data[];
} __attribute__((aligned(4)));

lctrCreateCisPend_t lctrMstCreateCisPend
    __attribute__((section(".ble_ll_bss"), aligned(2)));

static void ll_cis_master_msg_handler(u8 msg_id, void *payload);

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

static __attribute__((noinline)) u8 lctrPhyToPhysBit(u8 phy)
{
    return phy == 3 ? 4 : (phy == 2 ? 2 : 1);
}

static __inline__ __attribute__((always_inline)) u16 read_le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static __inline__ __attribute__((always_inline)) u32 read_le24(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

static void le_cmd_unpack_cig_params(LlCisCigParams_t *param, const u8 *data)
{
    param->cigId = data[0];
    param->sduIntervalMToS = read_le24(data + 1);
    param->sduIntervalSToM = read_le24(data + 4);
    param->sca = data[7];
    param->packing = data[8];
    param->framing = (LlFraming_t)data[9];
    param->transLatMToS = read_le16(data + 10);
    param->transLatSToM = read_le16(data + 12);
    param->numCis = data[14];
    for (u8 i = 0; i < param->numCis; i++) {
        const u8 *p = data + 15 + i * 9;
        param->pCisParam[i].cisId = p[0];
        param->pCisParam[i].sduSizeMToS = read_le16(p + 1);
        param->pCisParam[i].sduSizeSToM = read_le16(p + 3);
        param->pCisParam[i].phyMToS = p[5];
        param->pCisParam[i].phySToM = p[6];
        param->pCisParam[i].rteMToS = p[7];
        param->pCisParam[i].rteSToM = p[8];
    }
}

static void le_cmd_unpack_cig_params_test(LlCisCigParamsTest_t *param,
                                          const u8 *data)
{
    param->cigId = data[0];
    param->sduIntervalMToS = read_le24(data + 1);
    param->sduIntervalSToM = read_le24(data + 4);
    param->ftMToS = data[7];
    param->ftSToM = data[8];
    param->isoInterval = read_le16(data + 9);
    param->sca = data[11];
    param->packing = data[12];
    param->framing = (LlFraming_t)data[13];
    param->numCis = data[14];
    for (u8 i = 0; i < param->numCis; i++) {
        const u8 *p = data + 15 + i * 14;
        param->pCisParam[i].cisId = p[0];
        param->pCisParam[i].nse = p[1];
        param->pCisParam[i].sduSizeMToS = read_le16(p + 2);
        param->pCisParam[i].sduSizeSToM = read_le16(p + 4);
        param->pCisParam[i].pduSizeMToS = read_le16(p + 6);
        param->pCisParam[i].pduSizeSToM = read_le16(p + 8);
        param->pCisParam[i].phyMToS = p[10];
        param->pCisParam[i].phySToM = p[11];
        param->pCisParam[i].bnMToS = p[12];
        param->pCisParam[i].bnSToM = p[13];
    }
}

static u8 LlIsCigParamsValid(LlCisCigParams_t *pCigParam, int logging)
{
    if (pCigParam->cigId > 0xef) {
        return 0;
    }
    if (pCigParam->sduIntervalMToS != pCigParam->sduIntervalSToM) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid sduIntervalMToS != sduIntervalSToM\r\n");
        return 0;
    }
    if ((u32)(pCigParam->sduIntervalMToS - 255) > 1048320) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid sduIntervalMToS\r\n");
        return 0;
    }
    if (pCigParam->sca > 7) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid sca\r\n");
        return 0;
    }
    if (pCigParam->packing > 1) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid packing\r\n");
        return 0;
    }
    if (pCigParam->framing > 1) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid framing\r\n");
        return 0;
    }
    if ((u16)(pCigParam->transLatMToS - 5) > 3995) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid transLatMToS\r\n");
        return 0;
    }
    if ((u16)(pCigParam->transLatSToM - 5) > 3995) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid transLatSToM\r\n");
        return 0;
    }
    if ((u8)(pCigParam->numCis - 1) > 15) {
        if (logging)
            log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid numCis\r\n");
        return 0;
    }
    for (u8 i = 0; i < pCigParam->numCis; i++) {
        const LlCisCisParams_t *p = &pCigParam->pCisParam[i];
        if (p->cisId > 0xef) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid cisId\r\n");
            return 0;
        }
        if (p->sduSizeMToS > 4095) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid sduSizeMToS\r\n");
            return 0;
        }
        if (p->sduSizeSToM > 4095) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid sduSizeSToM\r\n");
            return 0;
        }
        if ((u8)(p->phyMToS - 1) > 6) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid phyMToS\r\n");
            return 0;
        }
        if ((u8)(p->phySToM - 1) > 6) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid phySToM\r\n");
            return 0;
        }
        if (p->rteMToS > 15) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid rteMToS\r\n");
            return 0;
        }
        if (p->rteSToM > 15) {
            if (logging)
                log_print(2, NULL, "[Info]: [LL_CIS]LlIsCigParamsValid Invalid rteSToM\r\n");
            return 0;
        }
    }
    return 1;
}

static u8 LlIsCigPhyParamsValid(LlCisCigParams_t *pCigParam)
{
    for (u8 i = 0; i < pCigParam->numCis; i++) {
        if (!llValidatePhySupport(pCigParam->pCisParam[i].phyMToS,
                                  pCigParam->pCisParam[i].phySToM))
            return 0;
    }
    return 1;
}

static u8 LlIsCigTestParamsValid(LlCisCigParamsTest_t *pCigParam, int logging)
{
    if (pCigParam->cigId > 0xef ||
        (u32)(pCigParam->sduIntervalMToS - 255) > 1048320 ||
        (u32)(pCigParam->sduIntervalSToM - 255) > 1048320 ||
        (u8)(pCigParam->ftMToS - 1) > 30 ||
        (u8)(pCigParam->ftSToM - 1) > 30 ||
        (u16)(pCigParam->isoInterval - 4) > 3196 ||
        pCigParam->sca > 7 ||
        (pCigParam->packing | pCigParam->framing) > 1 ||
        (u8)(pCigParam->numCis - 1) > 15)
        goto invalid;

    for (u8 i = 0; i < pCigParam->numCis; i++) {
        const LlCisCigCisParamsTest_t *p = &pCigParam->pCisParam[i];
        if (p->cisId > 0xef ||
            p->sduSizeMToS > 4095 ||
            p->sduSizeSToM > 4095 ||
            p->pduSizeMToS > 4091 ||
            p->pduSizeSToM > 4091 ||
            (u8)(p->phyMToS - 1) > 6 ||
            (u8)(p->phySToM - 1) > 6 ||
            p->bnMToS > 15 ||
            p->bnSToM > 15)
            goto invalid;
    }
    if (pCigParam->pCisParam[0].bnMToS > pCigParam->pCisParam[0].nse ||
        pCigParam->pCisParam[0].bnSToM > pCigParam->pCisParam[0].nse)
        goto invalid;
    return 1;

invalid:
    if (logging)
        log_print(2, NULL, "[Info]: [LL_CIS]Invalid Parameters\r\n");
    return 0;
}

static u8 LlIsCigPhyTestParamsValid(LlCisCigParamsTest_t *pCigParam)
{
    for (u8 i = 0; i < pCigParam->numCis; i++) {
        if (!llValidatePhySupport(pCigParam->pCisParam[i].phyMToS,
                                  pCigParam->pCisParam[i].phySToM))
            return 0;
    }
    return 1;
}

static void ll_set_cis(le_cis_t *cis, LlCisCigParams_t *pSetCigParam,
                       const LlCisCisParams_t *pCisParam) LL_CODE;
static void ll_set_cis(le_cis_t *cis, LlCisCigParams_t *pSetCigParam,
                       const LlCisCisParams_t *pCisParam)
{
    cis->cisId = pCisParam->cisId;
    cis->sca = pSetCigParam->sca;
    cis->packing = pSetCigParam->packing;
    cis->framing = pSetCigParam->framing;
    access_addr_generate((u8 *)&cis->accessAddr);

    if (log_tag_const_i_LL_CIS) {
        if (cis->framing == 0)
            log_print(2, NULL, "[Info]: [LL_CIS]SetCis LL_ISO_PDU_TYPE_UNFRAMED\r\n");
        else
            log_print(2, NULL, "[Info]: [LL_CIS]SetCis LL_ISO_PDU_TYPE_FRAMED\r\n");
    }

    if (pSetCigParam->sduIntervalMToS < 1251)
        pSetCigParam->sduIntervalMToS = 1250;
    if (pSetCigParam->sduIntervalSToM < 1251)
        pSetCigParam->sduIntervalSToM = 1250;

    cis->isoInterval = (u16)(pSetCigParam->sduIntervalMToS / 1250);
    cis->sduIntervalMToS = pSetCigParam->sduIntervalMToS;
    cis->sduIntervalSToM = pSetCigParam->sduIntervalSToM;
    cis->sduSizeMToS = pCisParam->sduSizeMToS;
    cis->sduSizeSToM = pCisParam->sduSizeSToM;
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]SetCis, sduMToS   =%d\r\n",
                  cis->sduSizeMToS);
    cis->pduSizeMToS = pCisParam->sduSizeMToS < ll_iso_hdl.maxIsoPduLen
                           ? pCisParam->sduSizeMToS : ll_iso_hdl.maxIsoPduLen;
    cis->pduSizeSToM = pCisParam->sduSizeSToM < ll_iso_hdl.maxIsoPduLen
                           ? pCisParam->sduSizeSToM : ll_iso_hdl.maxIsoPduLen;
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]SetCis, pduMToS   =%d\r\n",
                  cis->pduSizeMToS);
    cis->phyMToS = lctrPhysBitToPhy(ll_get_preferred_phy(pCisParam->phyMToS));
    cis->phySToM = lctrPhysBitToPhy(ll_get_preferred_phy(pCisParam->phySToM));

    u32 interval_usec = (u32)cis->isoInterval * 1250;
    cis->ftMToS = (u8)(((u32)pSetCigParam->transLatMToS * 1000) / interval_usec);
    cis->ftSToM = (u8)(((u32)pSetCigParam->transLatSToM * 1000) / interval_usec);

    u8 a = (u8)(interval_usec / cis->sduIntervalMToS);
    u8 b = (u8)ceil((double)cis->sduSizeMToS / cis->pduSizeMToS);
    cis->bnMToS = (u8)(b * (a == 0 ? 1 : a));

    a = (u8)(interval_usec / cis->sduIntervalSToM);
    b = (u8)ceil((double)cis->sduSizeSToM / cis->pduSizeSToM);
    cis->bnSToM = (u8)(b * (a == 0 ? 1 : a));

    u8 max_bn = cis->bnMToS > cis->bnSToM ? cis->bnMToS : cis->bnSToM;
    u8 max_rte = pCisParam->rteMToS > pCisParam->rteSToM
                     ? pCisParam->rteMToS
                     : pCisParam->rteSToM;
    cis->nse = (u8)(max_bn + max_rte);

    if (log_tag_const_i_LL_CIS) {
        log_print(2, NULL,
                  "[Info]: [LL_CIS]SetCis, bnMToS    =%d / bnSToM =%d\r\n",
                  cis->bnMToS, cis->bnSToM);
    }
}

static void ll_set_cis_test(le_cis_t *cis,
                            const LlCisCigParamsTest_t *pSetCigParam,
                            const LlCisCigCisParamsTest_t *pCisParam) LL_CODE;
static void ll_set_cis_test(le_cis_t *cis,
                            const LlCisCigParamsTest_t *pSetCigParam,
                            const LlCisCigCisParamsTest_t *pCisParam)
{
    cis->cisId = pCisParam->cisId;
    cis->sduIntervalMToS = pSetCigParam->sduIntervalMToS;
    cis->sduIntervalSToM = pSetCigParam->sduIntervalSToM;
    cis->ftMToS = pSetCigParam->ftMToS;
    cis->ftSToM = pSetCigParam->ftSToM;
    cis->isoInterval = pSetCigParam->isoInterval;
    cis->sca = pSetCigParam->sca;
    cis->packing = pSetCigParam->packing;
    cis->framing = pSetCigParam->framing;
    if (log_tag_const_i_LL_CIS) {
        if (cis->framing == 0)
            log_print(2, NULL, "[Info]: [LL_CIS]SetCisTest LL_ISO_PDU_TYPE_UNFRAMED\r\n");
        else
            log_print(2, NULL, "[Info]: [LL_CIS]SetCisTest LL_ISO_PDU_TYPE_FRAMED\r\n");
    }

    cis->nse = pCisParam->nse;
    cis->sduSizeMToS = pCisParam->sduSizeMToS;
    cis->sduSizeSToM = pCisParam->sduSizeSToM;
    cis->phyMToS = lctrPhysBitToPhy(ll_get_preferred_phy(pCisParam->phyMToS));
    cis->phySToM = lctrPhysBitToPhy(ll_get_preferred_phy(pCisParam->phySToM));
    cis->bnMToS = pCisParam->bnMToS;
    cis->bnSToM = pCisParam->bnSToM;

    int assert_policy = config_asser;
    cis_assert_failed_policy(cis->nse > (cis->bnMToS > cis->bnSToM
                                        ? cis->bnMToS
                                        : cis->bnSToM),
                      "ASSERT-FAILD: cis->nse > MAX(cis->bnMToS, cis->bnSToM) NSE shall be in the range max (BN_M_To_S, BN_S_To_M) to 31",
                      896, assert_policy);

    cis->pduSizeMToS = pCisParam->pduSizeMToS;
    cis->pduSizeSToM = pCisParam->pduSizeSToM;
    access_addr_generate((u8 *)&cis->accessAddr);
    struct le_link *link = ll_link_for_handle(cis->aclHandle);
    cis_assert_failed_policy(link != NULL, "ASSERT-FAILD: link ", 906, assert_policy);
    __builtin_memcpy(&cis->crcInit, link->ll_data.crcinit, 3);
    cis->supTimeoutMs = link->ll_data.timeout;
}

static __inline__ __attribute__((always_inline)) void cis_check_margin(
    le_cis_t *cis, const u8 *sca, int assert_policy, const char *diagnostic, int line)
{
    u32 subinterval = cis->subIntervUsec;
    u32 interval = (u32)cis->isoInterval * 2500;
    u16 accuracy = lctrCalcTotalAccuracy(*sca);
    u64 scaled = (u64)interval * accuracy;
    u64 limit = 150;
    if (scaled <= 150999999) {
        interval = (u32)cis->isoInterval * 2500;
        accuracy = lctrCalcTotalAccuracy(*sca);
        limit = (u64)interval * accuracy / 1000000;
    }
    cis_assert_failed_policy(subinterval > limit, diagnostic, line, assert_policy);
}

static u8 ll_set_cig_params(LlCisCigParams_t *param) LL_CODE;
static u8 ll_set_cig_params(LlCisCigParams_t *param)
{
    if (log_tag_const_i_LL_CIS) {
        log_print(2, NULL, "[Info]: [LL_CIS]CIG_ID                            %d\r\n",
                  param->cigId);
        log_print(2, NULL, "[Info]: [LL_CIS]SDU_Interval_M_To_S               0x%x\r\n",
                  param->sduIntervalMToS);
        log_print(2, NULL, "[Info]: [LL_CIS]SDU_Interval_S_To_M               0x%x\r\n",
                  param->sduIntervalSToM);
        log_print(2, NULL, "[Info]: [LL_CIS]Slaves_Clock_Accuracy             0x%x\r\n",
                  param->sca);
        log_print(2, NULL, "[Info]: [LL_CIS]Packing                           0x%x\r\n",
                  param->packing);
        log_print(2, NULL, "[Info]: [LL_CIS]Framing                           0x%x\r\n",
                  param->framing);
        log_print(2, NULL, "[Info]: [LL_CIS]Max_Transport_Latency_M_To_S      0x%x\r\n",
                  param->transLatMToS);
        log_print(2, NULL, "[Info]: [LL_CIS]Max_Transport_Latency_S_To_M      0x%x\r\n",
                  param->transLatSToM);
        log_print(2, NULL, "[Info]: [LL_CIS]CIS_Count                         0x%x\r\n",
                  param->numCis);
        for (u8 i = 0; i < param->numCis; i++) {
            LlCisCisParams_t *p = &param->pCisParam[i];
            log_print(2, NULL, "[Info]: [LL_CIS]CIS_ID[%d]            %d\r\n",
                      i, p->cisId);
            log_print(2, NULL, "[Info]: [LL_CIS]Max_SDU_M_To_S[%d]    0x%x\r\n",
                      i, p->sduSizeMToS);
            log_print(2, NULL, "[Info]: [LL_CIS]Max_SDU_S_To_M[%d]    0x%x\r\n",
                      i, p->sduSizeSToM);
            log_print(2, NULL, "[Info]: [LL_CIS]PHY_M_To_S[%d]        0x%x\r\n",
                      i, p->phyMToS);
            log_print(2, NULL, "[Info]: [LL_CIS]PHY_S_To_M[%d]        0x%x\r\n",
                      i, p->phySToM);
            log_print(2, NULL, "[Info]: [LL_CIS]RTN_M_To_S[%d]        0x%x\r\n",
                      i, p->rteMToS);
            log_print(2, NULL, "[Info]: [LL_CIS]RTN_S_To_M[%d]        0x%x\r\n",
                      i, p->rteSToM);
        }
    }
    if (!LlIsCigParamsValid(param, log_tag_const_i_LL_CIS != 0)) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]Invalid Parameters\r\n");
        return 48;
    }
    if (!LlIsCigPhyParamsValid(param)) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]Invalid PHY: Not supported by controller\r\n");
        return 17;
    }

    le_cig_t *cig = ll_cig_by_id(param->cigId);
    if (!cig)
        cig = ll_cig_alloc(param->cigId);
    if (!cig) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL, "[Debug]: [LL_CIS]SetCIG there is no more CIG context\r\n");
        return 7;
    }
    if (cig->numCisEsted != 0) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL, "[Debug]: [LL_CIS]SetCIG there is an established CIS\r\n");
        return 12;
    }
    if (ll_iso_hdl.createCisPend == 1) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL, "[Debug]: [LL_CIS]SetCIG there is a pending CIS\r\n");
        return 12;
    }

    u8 num_enable_cis = ll_get_number_enable_cis(param);
    if (num_enable_cis == 0) {
        if (param->numCis > ll_get_number_avail_cis()) {
            if (log_tag_const_d_LL_CIS)
                log_print(1, NULL, "[Debug]: [LL_CIS]there is no more CIS context\r\n");
            return 9;
        }
        for (u8 i = 0; i < param->numCis; i++) {
            le_cis_t *cis = ll_cis_alloc(cig);
            cis->role = LL_ROLE_MASTER;
            ll_set_cis(cis, param, &param->pCisParam[i]);
        }
    } else {
        if ((int)param->numCis - (int)num_enable_cis >
            (int)ll_get_number_avail_cis())
            return 7;
        for (u8 i = 0; i < param->numCis; i++) {
            LlCisCisParams_t *p = &param->pCisParam[i];
            le_cis_t *cis = ll_cis_by_id(param->cigId, p->cisId);
            if (cis) {
                ll_set_cis(cis, param, p);
            } else {
                cis = ll_cis_alloc(cig);
                cis->role = LL_ROLE_MASTER;
                ll_set_cis(cis, param, p);
            }
        }
    }

    int margin_assert_policy = config_asser;
    for (u8 i = 0; i < param->numCis; i++) {
        le_cis_t *cis = ll_cis_by_id(param->cigId, param->pCisParam[i].cisId);
        cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 948, margin_assert_policy);
        if (param->packing == LL_PACKING_INTERLEAVED) {
            cis->subIntervUsec = ll_cis_calc_subevt_interleaved_us(param);
            cis->delayUsec = ll_cis_calc_subevt_sequential_us(
                cis->phyMToS, cis->phySToM, (u8)cis->pduSizeMToS,
                (u8)cis->pduSizeSToM);
        } else {
            cis->subIntervUsec = ll_cis_calc_subevt_sequential_us(
                cis->phyMToS, cis->phySToM, (u8)cis->pduSizeMToS,
                (u8)cis->pduSizeSToM);
            cis->delayUsec = cis->subIntervUsec;
        }
        if (log_tag_const_i_LL_CIS) {
            log_print(2, NULL, "[Info]: [LL_CIS]cis->subIntervUsec 0x%x\r\n",
                      cis->subIntervUsec);
        }
        cis_check_margin(cis, &param->sca, margin_assert_policy,
                         "ASSERT-FAILD: (uint64_t)cis->subIntervUsec > MIN(LL_BLE_TMSS_US, (2 * (cis->isoInterval * 1250) * ((uint64_t)lctrCalcTotalAccuracy(pSetCigParam->sca))) / 1000000) ", 971);
        if (log_tag_const_i_LL_CIS) {
            log_print(2, NULL, "[Info]: [LL_CIS]SetCig,     subIntervUsec=%d\r\n", cis->subIntervUsec);
            log_print(2, NULL, "[Info]: [LL_CIS]SetCig,     delayUsec=%d\r\n", cis->delayUsec);
        }
    }

    le_cis_t *cis_first = NULL;
    if (param->packing == LL_PACKING_INTERLEAVED) {
        cis_first = ll_cis_by_id(param->cigId, param->pCisParam[0].cisId);
        int sync_assert_policy = config_asser;
        cis_assert_failed_policy(cis_first != NULL, "ASSERT-FAILD: cisFirst ", 981, sync_assert_policy);
        cis_first->cisSyncDelayUsec =
            cis_first->nse * cis_first->subIntervUsec;
        cis_first->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
        cig->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
        cig->isoInterval = cis_first->isoInterval;
        for (u8 i = 1; i < param->numCis; i++) {
            le_cis_t *cis = ll_cis_by_id(param->cigId,
                                         param->pCisParam[i].cisId);
            cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 989, sync_assert_policy);
            u32 duration = 0;
            for (u8 j = 0; j < i; j++) {
                le_cis_t *prior = ll_cis_by_id(param->cigId,
                                               param->pCisParam[j].cisId);
                duration += ll_cis_calc_subevt_sequential_us(
                    prior->phyMToS, prior->phySToM, (u8)prior->pduSizeMToS,
                    (u8)prior->pduSizeSToM);
            }
            cis->cisSyncDelayUsec = cis_first->cisSyncDelayUsec - duration;
            cis->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]LL_PACKING_INTERLEAVED, cis[%d] CIS_SyncDelayUsec=%d(us) / CIG_SyncDelayUsec=%d(us)\r\n",
                          i, cis->cisSyncDelayUsec, cis->cigSyncDelayUsec);
        }
    } else {
        for (u8 i = 0; i < param->numCis; i++) {
            le_cis_t *cis = ll_cis_by_id(param->cigId,
                                         param->pCisParam[i].cisId);
            cis_assert_failed(cis != NULL, "cis", 1007);
            if (i == 0)
                cis_first = cis;
            cis->cisSyncDelayUsec = 0;
            for (u8 j = i; j < param->numCis; j++) {
                le_cis_t *later = ll_cis_by_id(param->cigId,
                                                param->pCisParam[j].cisId);
                cis->cisSyncDelayUsec +=
                    later->nse * later->subIntervUsec;
            }
            cis->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]LL_PACKING_SEQUENTIAL, cis[%d] CIS_SyncDelayUsec=%d(us) / CIG_SyncDelayUsec=%d(us)\r\n",
                          i, cis->cigSyncDelayUsec, cis->cigSyncDelayUsec);
        }
        cig->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
        cig->isoInterval = cis_first->isoInterval;
    }
    cig->packing = param->packing;
    cig->isValid = cig->cigSyncDelayUsec <
                   ((u32)cig->isoInterval * 1250 - 150);
    cig->roleData.mst.numCis = param->numCis;
    return 0;
}

static u8 ll_set_cig_params_test(LlCisCigParamsTest_t *param) LL_CODE;
static u8 ll_set_cig_params_test(LlCisCigParamsTest_t *param)
{
    if (log_tag_const_i_LL_CIS) {
        log_print(2, NULL, "[Info]: [LL_CIS]CIG_ID            %d\r\n", param->cigId);
        log_print(2, NULL, "[Info]: [LL_CIS]sduIntervalMToS   0x%x\r\n",
                  param->sduIntervalMToS);
        log_print(2, NULL, "[Info]: [LL_CIS]sduIntervalSToM   0x%x\r\n",
                  param->sduIntervalSToM);
        log_print(2, NULL, "[Info]: [LL_CIS]ftMToS            0x%x\r\n", param->ftMToS);
        log_print(2, NULL, "[Info]: [LL_CIS]ftSToM            0x%x\r\n", param->ftSToM);
        log_print(2, NULL, "[Info]: [LL_CIS]isoInterval       0x%x\r\n",
                  param->isoInterval);
        log_print(2, NULL, "[Info]: [LL_CIS]sca               0x%x\r\n", param->sca);
        log_print(2, NULL, "[Info]: [LL_CIS]packing           0x%x\r\n",
                  param->packing);
        log_print(2, NULL, "[Info]: [LL_CIS]framing           0x%x\r\n",
                  param->framing);
        log_print(2, NULL, "[Info]: [LL_CIS]numCis            0x%x\r\n", param->numCis);
    }
    if (!LlIsCigTestParamsValid(param, log_tag_const_i_LL_CIS != 0)) {
        return 48;
    }
    if (!LlIsCigPhyTestParamsValid(param)) {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL,
                      "[Info]: [LL_CIS]Invalid PHY: Not supported by controller\r\n");
        return 17;
    }

    le_cig_t *cig = ll_cig_by_id(param->cigId);
    if (!cig)
        cig = ll_cig_alloc(param->cigId);
    if (!cig) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL,
                      "[Debug]: [LL_CIS]SetCIGTest there is no more CIG context\r\n");
        return 7;
    }
    if (cig->numCisEsted != 0) {
        if (log_tag_const_d_LL_CIS)
            log_print(1, NULL,
                      "[Debug]: [LL_CIS]SetCIGTest there is an established CIS\r\n");
        return 12;
    }

    u8 num_enable_cis =
        ll_get_number_enable_cis((LlCisCigParams_t *)param);
    if (num_enable_cis == 0) {
        if (param->numCis > ll_get_number_avail_cis()) {
            if (log_tag_const_d_LL_CIS)
                log_print(1, NULL,
                          "[Debug]: [LL_CIS]SetCIGTest there is no more CIS context\r\n");
            return 9;
        }
        for (u8 i = 0; i < param->numCis; i++) {
            le_cis_t *cis = ll_cis_alloc(cig);
            cis->role = LL_ROLE_MASTER;
            ll_set_cis_test(cis, param, &param->pCisParam[i]);
        }
    } else {
        if ((int)param->numCis - (int)num_enable_cis >
            (int)ll_get_number_avail_cis())
            return 7;
        for (u8 i = 0; i < param->numCis; i++) {
            LlCisCigCisParamsTest_t *p = &param->pCisParam[i];
            le_cis_t *cis = ll_cis_by_id(param->cigId, p->cisId);
            if (cis) {
                ll_set_cis_test(cis, param, p);
            } else {
                cis = ll_cis_alloc(cig);
                cis->role = LL_ROLE_MASTER;
                ll_set_cis_test(cis, param, p);
            }
        }
    }

    int margin_assert_policy = config_asser;
    for (u8 i = 0; i < param->numCis; i++) {
        le_cis_t *cis = ll_cis_by_id(param->cigId, param->pCisParam[i].cisId);
        cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 1050, margin_assert_policy);
        if (param->packing == LL_PACKING_INTERLEAVED) {
            cis->subIntervUsec = ll_cis_calc_subevt_interleaved_us(
                (LlCisCigParams_t *)param);
            cis->delayUsec = ll_cis_calc_subevt_sequential_us(
                cis->phyMToS, cis->phySToM, (u8)cis->pduSizeMToS,
                (u8)cis->pduSizeSToM);
        } else {
            cis->subIntervUsec = cis->nse > 1
                ? ll_cis_calc_subevt_sequential_us(
                      cis->phyMToS, cis->phySToM, (u8)cis->pduSizeMToS,
                      (u8)cis->pduSizeSToM)
                : 0;
            cis->delayUsec = cis->subIntervUsec;
        }
        cis_check_margin(cis, &param->sca, margin_assert_policy,
                         "ASSERT-FAILD: (uint64_t)cis->subIntervUsec > MIN(LL_BLE_TMSS_US, (2 * (cis->isoInterval * 1250) * ((uint64_t)lctrCalcTotalAccuracy(pSetCigParamTest->sca))) / 1000000) ", 1066);
    }

    le_cis_t *cis_first = NULL;
    if (param->packing == LL_PACKING_INTERLEAVED) {
        cis_first = ll_cis_by_id(param->cigId, param->pCisParam[0].cisId);
        int sync_assert_policy = margin_assert_policy;
        cis_assert_failed_policy(cis_first != NULL, "ASSERT-FAILD: cisFirst ", 1072, sync_assert_policy);
        cis_first->cisSyncDelayUsec =
            cis_first->nse * cis_first->subIntervUsec;
        cis_first->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
        cig->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
        cig->isoInterval = cis_first->isoInterval;
        for (u8 i = 1; i < param->numCis; i++) {
            le_cis_t *cis = ll_cis_by_id(param->cigId,
                                         param->pCisParam[i].cisId);
            cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 1080, sync_assert_policy);
            u32 duration = 0;
            for (u8 j = 0; j < i; j++) {
                le_cis_t *prior = ll_cis_by_id(param->cigId,
                                               param->pCisParam[j].cisId);
                duration += ll_cis_calc_subevt_sequential_us(
                    prior->phyMToS, prior->phySToM, (u8)prior->pduSizeMToS,
                    (u8)prior->pduSizeSToM);
            }
            cis->cisSyncDelayUsec = cis_first->cisSyncDelayUsec - duration;
            cis->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]LL_PACKING_INTERLEAVED, cis[%d] CIS_SyncDelayUsec=%d(us) / CIG_SyncDelayUsec=%d(us)\r\n",
                          i, cis->cisSyncDelayUsec, cis->cigSyncDelayUsec);
        }
    } else {
        for (u8 i = 0; i < param->numCis; i++) {
            le_cis_t *cis = ll_cis_by_id(param->cigId,
                                         param->pCisParam[i].cisId);
            cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 1102, margin_assert_policy);
            if (i == 0)
                cis_first = cis;
            cis->cisSyncDelayUsec = 0;
            for (u8 j = i; j < param->numCis; j++) {
                le_cis_t *later = ll_cis_by_id(param->cigId,
                                                param->pCisParam[j].cisId);
                cis->cisSyncDelayUsec +=
                    later->nse * later->subIntervUsec;
            }
            cis->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]LL_PACKING_SEQUENTIAL, cis[%d] CIS_SyncDelayUsec=%d(us) / CIG_SyncDelayUsec=%d(us)\r\n",
                          i, cis->cigSyncDelayUsec, cis->cigSyncDelayUsec);
        }
        cig->cigSyncDelayUsec = cis_first->cisSyncDelayUsec;
        cig->isoInterval = cis_first->isoInterval;
    }
    cig->packing = param->packing;
    cig->isValid = cig->cigSyncDelayUsec <
                   ((u32)cig->isoInterval * 1250 - 150);
    cig->roleData.mst.numCis = param->numCis;
    return 0;
}

static void lctrCalCeRefFirstCis(struct le_link *link, le_cis_t *cis) LL_CODE;
static void lctrCalCeRefFirstCis(struct le_link *link, le_cis_t *cis)
{
    cis->ceRef = (u16)(link->event_cnt + 7 + link->ll_data.latency);
    cis->cisCeRef = 0;
}

static void lctrCalCeRefNotFirstCis(struct le_link *link, le_cis_t *cis) LL_CODE;
static void lctrCalCeRefNotFirstCis(struct le_link *link, le_cis_t *cis)
{
    le_cig_t *cig = cis->cig;
    lctrCalCeRefFirstCis(link, cis);
    cis->cisSyncDelayUsec = cis->cigSyncDelayUsec - 1476;
    (void)cig;
}

static u8 ll_create_cis(LlCisCreateCisParams_t *pCreateCisParam) LL_CODE;
static u8 ll_create_cis(LlCisCreateCisParams_t *pCreateCisParam)
{
    for (u8 i = 0; i < pCreateCisParam->numCis; i++) {
        u16 cis_handle = pCreateCisParam->pCisParam[i].cisHandle;
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]CIS[%d] handle 0x%x\r\n",
                      i, cis_handle);
        le_cis_t *cis = ll_cis_by_handle(cis_handle);
        if (!cis) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL,
                          "<Error>: [LL_CIS]CreateCis, invalid CIS handle\r\n");
            return 2;
        }
        if (ll_cis_is_established(cis) == LCTR_CIS_STATE_EST) {
            if (log_tag_const_d_LL_CIS)
                log_print(1, NULL,
                          "[Debug]: [LL_CIS]CreateCis, CIS is already established\r\n");
            return 11;
        }
    }

    ll_iso_hdl.createCisPend = 1;
    lctrMstCreateCisPend.numCis = pCreateCisParam->numCis;
    for (u8 i = 0; i < pCreateCisParam->numCis; i++) {
        lctrMstCreateCisPend.cisHandle[i] =
            pCreateCisParam->pCisParam[i].cisHandle;
        lctrMstCreateCisPend.aclHandle[i] =
            pCreateCisParam->pCisParam[i].aclHandle;
        lctrMstCreateCisPend.isCreateCisDone[i] = 0;
    }

    u16 acl_handle = pCreateCisParam->pCisParam[0].aclHandle;
    u16 cis_handle = pCreateCisParam->pCisParam[0].cisHandle;
    u8 *message = ll_iso_msg_alloc(4);
    msg_alloc_assert(message, 1423);
    *(cis_word *)message = 13;
    *(cis_word *)(message + 2) = LL_CIS_MST_MSG_CREATE_CIS;
    *(cis_word *)(message + 4) = acl_handle;
    *(cis_word *)(message + 6) = cis_handle;
    ll_iso_msg_send(message);
    return 0;
}

static u8 ll_create_cises(u8 *data) LL_CODE;
static u8 ll_create_cises(u8 *data)
{
    LlCisCreateCisParams_t *param = (LlCisCreateCisParams_t *)data;
    if (log_tag_const_i_LL_CIS) {
        log_print(2, NULL, "[Info]: [LL_CIS]numCis %d\r\n", param->numCis);
    }

    if (log_tag_const_i_LL_CIS) {
        for (u32 i = 0; i < param->numCis; i++) {
            log_print(2, NULL, "[Info]: [LL_CIS]Cis[%d] cisHandle 0x%x\r\n", i, param->pCisParam[i].cisHandle);
            log_print(2, NULL, "[Info]: [LL_CIS]Cis[%d] aclHandle 0x%x\r\n", i, param->pCisParam[i].aclHandle);
        }
    }
    if ((config_btctler_le_features & ((u64)1 << 32)) == 0) {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL,
                      "[Info]: [LL_CIS]CreateCISes : LL_FEAT_ISO_HOST_SUPPORT No\r\n");
        return 12;
    }

    for (u8 i = 0; i < param->numCis; i++) {
        u16 acl_handle = param->pCisParam[i].aclHandle;
        struct le_link *link = ll_link_for_handle(acl_handle);
        if (!link) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL,
                          "<Error>: [LL_CIS]CreateCises : UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        if (link->role == LL_ROLE_SLAVE) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL,
                          "<Error>: [LL_CIS]CreateCises : COMMAND_DISALLOWED\r\n");
            return 12;
        }
    }
    if (ll_iso_hdl.createCisPend == 1) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL,
                      "<Error>: [LL_CIS]CreateCis : LL_ERROR_CODE_CMD_DISALLOWED\r\n");
        return 12;
    }
    return ll_create_cis(param);
}

static u8 ll_remove_cig(u8 cig_id) LL_CODE;
static u8 ll_remove_cig(u8 cig_id)
{
    le_cig_t *cig = ll_cig_by_id(cig_id);
    if (!cig) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]invalid CIG_ID=%d.\r\n", cig_id);
        return 12;
    }

    for (struct list_head *node = cig->cis_head.next;
         node != &cig->cis_head; node = node->next) {
        le_cis_t *cis = (le_cis_t *)node;
        if (cis->cig != cig)
            continue;
        if (cis->role == LL_ROLE_SLAVE) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL, "<Error>: [LL_CIS]invalid role\r\n");
            return 12;
        }
        if (cis->isCisReqPend == 1) {
            if (log_tag_const_e_LL_CIS)
                log_print(4, NULL,
                          "<Error>: [LL_CIS]LctrRemoveCig, there is a pending CIS\r\n");
            return 12;
        }
    }
    if (cig->numCisEsted != 0) {
        if (log_tag_const_e_LL_CIS)
            log_print(4, NULL, "<Error>: [LL_CIS]there is an established CIS\r\n");
        return 12;
    }
    for (struct list_head *node = cig->cis_head.next;
         node != &cig->cis_head;) {
        struct list_head *next = node->next;
        le_cis_t *cis = (le_cis_t *)node;
        if (cis->cig == cig && cis->role == LL_ROLE_MASTER)
            ll_cis_free(cis);
        node = next;
    }
    ll_cig_free(cig);
    return 0;
}

static u8 le_evt_pack_cig_params(u8 *payload, u8 status, u8 cig_id,
                                 u8 num_cis) LL_CODE;
static u8 le_evt_pack_cig_params(u8 *payload, u8 status, u8 cig_id,
                                 u8 num_cis)
{
    payload[0] = status;
    payload[1] = cig_id;
    payload[2] = num_cis;
    u8 *cursor = payload + 3;
    for (struct list_head *node = ll_iso_hdl.cis_head.next;
         node != &ll_iso_hdl.cis_head; node = node->next) {
        le_cis_t *cis = (le_cis_t *)node;
        *cursor++ = (u8)cis->cisHandle;
        *cursor++ = (u8)(cis->cisHandle >> 8);
    }
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]CIG params size %d\r\n",
                  (num_cis << 1) + 3);
    return (u8)((num_cis << 1) + 3);
}

static void llcp_master_create_cis_done(le_cis_t *cis) LL_CODE;
static void llcp_master_create_cis_done(le_cis_t *cis)
{
    cis->isCisReqPend = 0;
    u8 num_cis = lctrMstCreateCisPend.numCis;
    for (u8 i = 0; i < num_cis; i++) {
        if (cis->cisHandle == lctrMstCreateCisPend.cisHandle[i]) {
            lctrMstCreateCisPend.isCreateCisDone[i] = 1;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]CIS[%d] created 0x%x\r\n",
                          i, cis->cisHandle);
            break;
        }
    }

    num_cis = lctrMstCreateCisPend.numCis;
    for (u32 i = 0; i < num_cis; i++) {
        if (lctrMstCreateCisPend.isCreateCisDone[i] == 0) {
            u16 acl_handle = lctrMstCreateCisPend.aclHandle[i];
            u16 cis_handle = lctrMstCreateCisPend.cisHandle[i];
            u8 *message = ll_iso_msg_alloc(4);
            msg_alloc_assert(message, 473);
            *(cis_word *)message = 13;
            *(cis_word *)(message + 2) = LL_CIS_MST_MSG_CREATE_CIS;
            *(cis_word *)(message + 4) = acl_handle;
            *(cis_word *)(message + 6) = cis_handle;
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]CIS[%d] next create 0x%x\r\n",
                          i, cis_handle);
            ll_iso_msg_send(message);
            return;
        }
    }

    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]CIS create all done\r\n");
    ll_iso_hdl.createCisPend = 0;
}

static u8 llcp_master_send_cis_req(struct le_link *link) LL_CODE;
static u8 llcp_master_send_cis_req(struct le_link *link)
{
    le_cis_t *cis = ll_cis_by_handle(link->cisHandle);
    le_cig_t *cig = cis->cig;
    ll_send_control_data(
        link, LL_PDU_CIS_REQ, "1111223322131112332",
        cig->cigId, cis->cisId,
        lctrPhyToPhysBit(cis->phyMToS), lctrPhyToPhysBit(cis->phySToM),
        (cis->framing << 15) | cis->sduSizeMToS,
        cis->sduSizeSToM, cis->sduIntervalMToS, cis->sduIntervalSToM,
        cis->pduSizeMToS, cis->pduSizeSToM, cis->nse, cis->subIntervUsec,
        (cis->bnSToM << 4) | cis->bnMToS, cis->ftMToS, cis->ftSToM,
        cis->isoInterval, cis->offsetUsec, cis->offsetUsec, cis->ceRef);
    cis->isCisReqPend = 1;
    return 0;
}

static void llcp_master_send_cis_ind(struct le_link *link,
                                     le_cis_t *cis) LL_CODE;
static void llcp_master_send_cis_ind(struct le_link *link, le_cis_t *cis)
{
    cis->isCisReqPend = 0;
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]-> LL_PDU_CIS_IND\r\n");
    ll_response_timeout_stop(link);
    ll_send_control_data(link, LL_PDU_CIS_IND, "43332", cis->accessAddr,
                         cis->offsetUsec, cis->cigSyncDelayUsec,
                         cis->cisSyncDelayUsec, cis->ceRef);
}

static u8 llcp_master_cis_rx(struct le_link *link,
                             const u8 *cis_rsp, int *assert_policy) LL_CODE;
static u8 llcp_master_cis_rx(struct le_link *link, const u8 *cis_rsp, int *assert_policy)
{
    le_cis_t *cis = ll_cis_by_handle(link->cisHandle);
    *assert_policy = config_asser;
    cis_assert_failed_policy(cis != NULL, "ASSERT-FAILD: cis ", 246, *assert_policy);

    u32 cis_off_min = *(const cis_dword *)cis_rsp;
    u32 cis_off_max = *(const cis_dword *)(cis_rsp + 4);
    if (cis_off_max < cis_off_min) {
        ll_send_control_data(link, LL_REJECT_IND_EXT, "11",
                             LL_PDU_CIS_RSP, 13);
        ll_response_timeout_stop(link);
        cis->isCisReqPend = 0;
        u8 *message = ll_iso_msg_alloc(4);
        cis_assert_failed_policy(message != NULL, "ASSERT-FAILD: msg ", 264, *assert_policy);
        *(cis_word *)message = 12;
        *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_FAIL;
        *(cis_word *)(message + 4) = link->cisHandle;
        *(u8 *)(message + 6) = 13;
        ll_iso_msg_send(message);
    } else {
        llcp_master_send_cis_ind(link, cis);
    }
    return 0;
}

void ll_cis_master_send_rx_llcp_msg(struct le_link *link,
                                    struct ble_rx *rx) LL_CODE;
void ll_cis_master_send_rx_llcp_msg(struct le_link *link, struct ble_rx *rx)
{
    u8 opcode = rx->data[0];
    if (opcode == LL_PDU_CIS_RSP) {
        int assert_policy = config_asser;
        u8 *message = ll_iso_msg_alloc(56);
        cis_assert_failed_policy(message != NULL, "ASSERT-FAILD: msg ", 1854, assert_policy);
        opcode = rx->data[0];
        *(cis_word *)message = 13;
        *(cis_word *)(message + 2) = LL_CIS_MST_MSG_RX_LLCP;
        *(u8 *)(message + 6) = opcode;
        *(cis_word *)(message + 4) = link->handle;
        if (opcode == LL_PDU_CIS_RSP) {
            *(cis_dword *)(message + 8) = read_le24(rx->data + 1);
            *(cis_dword *)(message + 12) = read_le24(rx->data + 4);
            *(cis_word *)(message + 16) = read_le16(rx->data + 7);
        }
        ll_iso_msg_send(message);
    }
}

void ll_cis_master_init(void) LL_CODE;
void ll_cis_master_init(void)
{
    ll_dispatch_msg_table[13] = ll_cis_master_msg_handler;
}

static void llcp_master_create_cis(struct cis_ll_msg *payload) LL_CODE;
static void llcp_master_create_cis(struct cis_ll_msg *payload)
{
    u16 acl_handle = *(cis_word *)payload;
    u16 cis_handle = *(cis_word *)((u8 *)payload + 2);
    struct le_link *link = ll_link_for_handle(acl_handle);
    le_cis_t *cis = ll_cis_by_handle(cis_handle);
    le_cig_t *cig = cis->cig;
    link->cisHandle = cis_handle;
    ll_response_timeout_start(link);

    if (cig->numCisEsted == 0) {
        if (cig->isValid == 0) {
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL,
                          "[Info]: [LL_CIS]Fail to create CIS due to invalid parameters\r\n\r\n");
            u8 *message = ll_iso_msg_alloc(4);
            msg_alloc_assert(message, 401);
            *(cis_word *)message = 12;
            *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_FAIL;
            *(cis_word *)(message + 4) = link->cisHandle;
            *(u8 *)(message + 6) = 13;
            ll_iso_msg_send(message);
            return;
        }
        lctrCalCeRefFirstCis(link, cis);
        cis->offsetUsec = 1356;
    } else {
        cis_assert_failed(cis->cig != NULL, "cig", 137);
        lctrCalCeRefNotFirstCis(link, cis);
        cis->cisSyncDelayUsec = cis->cigSyncDelayUsec - 1476;
        cis->offsetUsec = 1476;
    }
    __builtin_memcpy(&cis->crcInit, link->ll_data.crcinit, 3);
    cis->supTimeoutMs = link->ll_data.timeout;
    if (log_tag_const_i_LL_CIS)
        log_print(2, NULL, "[Info]: [LL_CIS]-> LL_PDU_CIS_REQ\r\n");
    llcp_master_send_cis_req(link);
    cis->llcp_state = 0;
}

static void ll_cis_master_msg_handler(u8 msg_id, void *payload) LL_CODE;
static void ll_cis_master_msg_handler(u8 msg_id, void *payload)
{
    struct cis_ll_msg *data = payload;
    switch (msg_id) {
    case LL_CIS_MST_MSG_CREATE_CIS:
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS] -->LL_CIS_MST_MSG_CREATE_CIS\r\n");
        llcp_master_create_cis(payload);
        break;
    case LL_CIS_MST_MSG_RX_LLCP: {
        struct le_link *link = ll_link_for_handle(data->handle);
        le_cis_t *cis = ll_cis_by_handle(link->cisHandle);
        switch (data->opcode) {
        case LL_PDU_CIS_RSP:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_PDU_CIS_RSP\r\n");
            if (cis->llcp_state != 0)
                break;
            {
            int assert_policy;
            llcp_master_cis_rx(link, data->data, &assert_policy);
            cis->llcp_state = 2;
            {
                u8 *message = ll_iso_msg_alloc(2);
                cis_assert_failed_policy(message != NULL, "ASSERT-FAILD: msg ", 1723, assert_policy);
                *(cis_word *)message = 12;
                *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_SUCCESS;
                *(cis_word *)(message + 4) = link->cisHandle;
                ll_iso_msg_send(message);
            }
            llcp_master_create_cis_done(cis);
            }
            break;
        case LL_UNKNOWN_RSP:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_UNKNOWN_RSP\r\n");
            if (data->data[0] != LL_PDU_CIS_REQ)
                break;
            {
                u8 *message = ll_iso_msg_alloc(4);
                msg_alloc_assert(message, 1747);
                *(cis_word *)message = 12;
                *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_FAIL;
                *(cis_word *)(message + 4) = link->cisHandle;
                *(u8 *)(message + 6) = 26;
                ll_iso_msg_send(message);
            }
            llcp_master_create_cis_done(cis);
            break;
        case LL_REJECT_IND:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_REJECT_IND\r\n");
            {
                u8 *message = ll_iso_msg_alloc(4);
                msg_alloc_assert(message, 1770);
                *(cis_word *)message = 12;
                *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_FAIL;
                *(cis_word *)(message + 4) = link->cisHandle;
                *(u8 *)(message + 6) = data->data[0];
                ll_iso_msg_send(message);
            }
            llcp_master_create_cis_done(cis);
            break;
        case LL_REJECT_IND_EXT:
            if (log_tag_const_i_LL_CIS)
                log_print(2, NULL, "[Info]: [LL_CIS]--> LL_REJECT_IND_EXT\r\n");
            if (data->data[0] != LL_PDU_CIS_REQ)
                break;
            {
                u8 *message = ll_iso_msg_alloc(4);
                msg_alloc_assert(message, 1794);
                *(cis_word *)message = 12;
                *(cis_word *)(message + 2) = LL_CIS_MSG_CREATE_CIS_FAIL;
                *(cis_word *)(message + 4) = link->cisHandle;
                *(u8 *)(message + 6) = data->data[1];
                ll_iso_msg_send(message);
            }
            llcp_master_create_cis_done(cis);
            break;
        }
        break;
    }
    }
}

int le_cmd_set_cis_master(u16 opcode, const u8 *data, u32 size) LL_CODE;
int le_cmd_set_cis_master(u16 opcode, const u8 *data, u32 size)
{
    (void)size;
    u16 ocf = opcode & 0x3ff;
    switch (ocf) {
    case 0x62: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> HCI_LE_SET_CIG_PARAMS\r\n");
        LlCisCigParams_t param;
        __builtin_memset(&param, 0, sizeof(param));
        le_cmd_unpack_cig_params(&param, data);
        u8 status = ll_set_cig_params(&param);
        u8 packed_size = le_evt_pack_cig_params((u8 *)data, status,
                                                 param.cigId, param.numCis);
        hci_send_event_cmd_complete_payload(opcode, (u8 *)data, packed_size);
        return 1;
    }
    case 0x63: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL,
                      "[Info]: [LL_CIS]-> HCI_LE_SET_CIG_PARAMS_TEST\r\n");
        LlCisCigParamsTest_t param;
        __builtin_memset(&param, 0, sizeof(param));
        le_cmd_unpack_cig_params_test(&param, data);
        u8 status = ll_set_cig_params_test(&param);
        u8 packed_size = le_evt_pack_cig_params((u8 *)data, status,
                                                 param.cigId, param.numCis);
        hci_send_event_cmd_complete_payload(opcode, (u8 *)data, packed_size);
        return 1;
    }
    case 0x64: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> HCI_LE_CREATE_CIS\r\n");
        u8 status = ll_create_cises((u8 *)data);
        hci_send_event(15, "112", status, 1, opcode);
        return 1;
    }
    case 0x65: {
        if (log_tag_const_i_LL_CIS)
            log_print(2, NULL, "[Info]: [LL_CIS]-> HCI_LE_REMOVE_CIG\r\n");
        u8 cig_id = data[0];
        u8 status = ll_remove_cig(cig_id);
        hci_send_event_cmd_complete(opcode, "11", status, cig_id);
        return 1;
    }
    default:
        return 0;
    }
}

#ifdef UNITTEST
#include "test.h"

#include <stdarg.h>

ll_iso_handle_t ll_iso_hdl;
ll_dispatch_msg_t ll_dispatch_msg_table[15];
u8 log_tag_const_d_LL_CIS, log_tag_const_e_LL_CIS, log_tag_const_i_LL_CIS;
u64 config_btctler_le_features;
int config_asser;

static le_cis_t fixture_cis[6];
static le_cig_t fixture_cig;
static struct le_link fixture_link;
static int active_cis, enabled_cis, available_cis = 6;
static int debug_asserts, resets, control_opcode, control_calls, control_args[24];
static int starts, stops, sent_count, hci_status, hci_opcode, hci_size;
static u8 messages[8][64], message[64], hci_payload[64];
static u16 message_size;
static u16 accuracy_value = 100;
static int accuracy_calls, accuracy_mutation;
static le_cis_t *accuracy_cis;
static int mutate_done, mutate_log;
static LlCisCisParams_t *mutate_params;
static struct ble_rx *mutate_rx;

struct le_link *ll_link_for_handle(int handle) { return handle == fixture_link.handle ? &fixture_link : NULL; }
le_cis_t *ll_cis_by_handle(u16 handle)
{
    for (int i = 0; i < active_cis; i++) if (fixture_cis[i].cisHandle == handle) return &fixture_cis[i];
    return NULL;
}
le_cis_t *ll_cis_by_id(u8 cig_id, u8 cis_id)
{
    (void)cig_id;
    for (int i = 0; i < active_cis; i++) if (fixture_cis[i].cisId == cis_id) return &fixture_cis[i];
    return NULL;
}
le_cig_t *ll_cig_by_id(u8 id) { return id == fixture_cig.cigId ? &fixture_cig : NULL; }
le_cig_t *ll_cig_alloc(u8 id) { fixture_cig.cigId = id; return &fixture_cig; }
le_cis_t *ll_cis_alloc(le_cig_t *cig)
{
    CHECK(active_cis < 6); le_cis_t *cis = &fixture_cis[active_cis++];
    memset(cis, 0, sizeof(*cis)); cis->cig = cig; cis->cisHandle = 100 + active_cis;
    cis->aclHandle = fixture_link.handle; return cis;
}
void ll_cis_free(le_cis_t *cis) { (void)cis; }
void ll_cig_free(le_cig_t *cig) { CHECK(cig == &fixture_cig); }
u8 ll_get_number_enable_cis(LlCisCigParams_t *p) { (void)p; return enabled_cis; }
u8 ll_get_number_avail_cis(void) { return available_cis; }
u8 llValidatePhySupport(u8 tx, u8 rx) { return tx != 0 && rx != 0; }
u8 ll_cis_is_established(le_cis_t *cis) { return cis->state; }
u32 ll_cis_calc_subevt_sequential_us(u8 tx, u8 rx, u8 tx_len, u8 rx_len)
{ (void)tx; (void)rx; (void)tx_len; (void)rx_len; return 200; }
u32 ll_cis_calc_subevt_interleaved_us(LlCisCigParams_t *p) { (void)p; return 300; }
u16 lctrCalcTotalAccuracy(u8 sca)
{
    (void)sca; accuracy_calls++;
    if (accuracy_mutation) {
        accuracy_cis->isoInterval = 61; accuracy_cis->subIntervUsec = 9999; config_asser = 0;
        return accuracy_calls == 1 ? 1006 : 2000;
    }
    return accuracy_value;
}
void *ll_iso_msg_alloc(u16 size)
{
    message_size = size; memset(message, 0xaa, sizeof(message));
    if (mutate_rx) { *((u8 *)mutate_rx + offsetof(struct ble_rx, data)) = 0xff; mutate_rx = NULL; }
    return message;
}
void ll_iso_msg_send(void *p)
{ CHECK(p == message && sent_count < 8); memcpy(messages[sent_count++], p, sizeof(message)); }
void ll_response_timeout_start(struct le_link *link) { CHECK(link == &fixture_link); starts++; }
void ll_response_timeout_stop(struct le_link *link) { CHECK(link == &fixture_link); stops++; }
void ll_send_control_data(struct le_link *link, int opcode, const char *format, ...)
{
    CHECK(link == &fixture_link); control_opcode = opcode; control_calls++;
    va_list ap; va_start(ap, format);
    for (size_t i = 0; i < strlen(format); i++) control_args[i] = va_arg(ap, int);
    va_end(ap);
}
int hci_send_event(int event, const char *format, ...)
{
    CHECK(event == 15 && !strcmp(format, "112")); va_list ap; va_start(ap, format);
    hci_status = va_arg(ap, int); CHECK(va_arg(ap, int) == 1); hci_opcode = va_arg(ap, int); va_end(ap); return 0;
}
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    CHECK(!strcmp(format, "11")); hci_opcode = opcode; va_list ap; va_start(ap, format);
    hci_status = va_arg(ap, int); (void)va_arg(ap, int); va_end(ap); return 0;
}
int hci_send_event_cmd_complete_payload(int opcode, u8 *payload, u8 size)
{
    CHECK(size <= sizeof(hci_payload)); hci_opcode = opcode; hci_size = size;
    memcpy(hci_payload, payload, size); hci_status = payload[0]; return 0;
}
static void reset_fixture(void)
{
    memset(fixture_cis, 0, sizeof(fixture_cis)); memset(&fixture_cig, 0, sizeof(fixture_cig));
    memset(&fixture_link, 0, sizeof(fixture_link)); memset(&lctrMstCreateCisPend, 0, sizeof(lctrMstCreateCisPend));
    active_cis = enabled_cis = debug_asserts = resets = control_calls = sent_count = starts = stops = 0;
    fixture_link.handle = 1; fixture_link.event_cnt = 10; fixture_link.ll_data.latency = 2;
    fixture_cig.cigId = 7; fixture_cig.cis_head.next = fixture_cig.cis_head.prev = &fixture_cig.cis_head;
    ll_iso_hdl.cis_head.next = ll_iso_hdl.cis_head.prev = &ll_iso_hdl.cis_head;
    ll_iso_hdl.createCisPend = 0; ll_iso_hdl.maxIsoSduLen = 999; ll_iso_hdl.maxIsoPduLen = 100;
    config_btctler_le_features = (u64)1 << 32; config_asser = 1; available_cis = 6;
    log_tag_const_i_LL_CIS = log_tag_const_d_LL_CIS = log_tag_const_e_LL_CIS = 0;
}

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    if (mutate_log && strstr(format, "SetCis, sduMToS")) {
        mutate_params->sduSizeMToS = 40; mutate_params->sduSizeSToM = 50; ll_iso_hdl.maxIsoPduLen = 32;
    }
    if (mutate_done && strstr(format, "created 0x")) lctrMstCreateCisPend.numCis = 0;
}

u8 ll_get_preferred_phy(u8 phys)
{
    return phys;
}

void access_addr_generate(u8 *address)
{
    memset(address, 0x5a, 4);
}

void p33_soft_reset(void) { resets++; }

void cpu_assert_debug(void) { debug_asserts++; }

int main(void)
{
    u8 command[15 + 9] = {0};
    command[0] = 7;
    command[1] = 0x88;
    command[2] = 0x13;
    command[3] = 0;
    command[4] = 0x88;
    command[5] = 0x13;
    command[6] = 0;
    command[7] = 2;
    command[8] = LL_PACKING_INTERLEAVED;
    command[9] = LL_ISO_PDU_TYPE_FRAMED;
    command[10] = 0x68;
    command[11] = 0x10;
    command[12] = 0x68;
    command[13] = 0x10;
    command[14] = 1;
    command[15] = 3;
    command[16] = 0x68;
    command[17] = 0x03;
    command[18] = 0x68;
    command[19] = 0x03;
    command[20] = 2;
    command[21] = 4;
    command[22] = 1;
    command[23] = 2;

    LlCisCigParams_t param;
    memset(&param, 0, sizeof(param));
    le_cmd_unpack_cig_params(&param, command);
    CHECK(param.cigId == 7 && param.sduIntervalMToS == 5000);
    CHECK(param.sduIntervalSToM == 5000 && param.sca == 2);
    CHECK(param.packing == LL_PACKING_INTERLEAVED);
    CHECK(param.framing == LL_ISO_PDU_TYPE_FRAMED);
    CHECK(param.transLatMToS == 4200 && param.transLatSToM == 4200);
    CHECK(param.numCis == 1 && param.pCisParam[0].cisId == 3);
    CHECK(param.pCisParam[0].sduSizeMToS == 872);
    CHECK(param.pCisParam[0].sduSizeSToM == 872);
    CHECK(param.pCisParam[0].phyMToS == 2 && param.pCisParam[0].phySToM == 4);
    CHECK(param.pCisParam[0].rteMToS == 1 && param.pCisParam[0].rteSToM == 2);

    ll_iso_hdl.maxIsoPduLen = 800;
    le_cis_t cis;
    memset(&cis, 0, sizeof(cis));
    ll_set_cis(&cis, &param, &param.pCisParam[0]);
    CHECK(cis.cisId == 3 && cis.framing == LL_ISO_PDU_TYPE_FRAMED);
    CHECK(cis.isoInterval == 4 && cis.sduIntervalMToS == 5000);
    CHECK(cis.sduSizeMToS == 872 && cis.pduSizeMToS == 800);
    CHECK(cis.phyMToS == 2 && cis.phySToM == 3);
    CHECK(cis.ftMToS == (u8)840 && cis.ftSToM == (u8)840);
    CHECK(cis.bnMToS == 2 && cis.bnSToM == 2 && cis.nse == 4);
    CHECK(cis.accessAddr == 0x5a5a5a5a);

    u8 response[32] = {0};
    cis.cisHandle = 0x1234;
    ll_iso_hdl.cis_head.next = &cis.entry;
    ll_iso_hdl.cis_head.prev = &cis.entry;
    cis.entry.next = &ll_iso_hdl.cis_head;
    cis.entry.prev = &ll_iso_hdl.cis_head;
    CHECK(le_evt_pack_cig_params(response, 0, 7, 1) == 5);
    CHECK(response[0] == 0 && response[1] == 7 && response[2] == 1);
    CHECK(response[3] == 0x34 && response[4] == 0x12);

    memset(ll_dispatch_msg_table, 0, sizeof(ll_dispatch_msg_table));
    ll_cis_master_init();
    CHECK(ll_dispatch_msg_table[13] == ll_cis_master_msg_handler);
    /* Margin snapshots precede callbacks; 150900000 still takes the second
     * accuracy call. A changed interval/accuracy makes the limit 305, so the
     * captured 160 fails despite callback replacing the field with 9999. */
    reset_fixture(); memset(&cis, 0, sizeof(cis));
    cis.isoInterval = 60; cis.subIntervUsec = 160; accuracy_cis = &cis;
    accuracy_mutation = 1; accuracy_calls = 0; u8 sca = 0;
    cis_check_margin(&cis, &sca, 1, "margin fixture", 971);
    CHECK(accuracy_calls == 2 && debug_asserts == 1 && resets == 0);
    accuracy_mutation = 0;

    /* SDK NSE assertion is strictly greater than both BNs, then continues
     * after a returning failure hook. */
    reset_fixture(); LlCisCigParamsTest_t test_param = {0};
    test_param.ftMToS = test_param.ftSToM = 1; test_param.isoInterval = 4;
    test_param.pCisParam[0].nse = 3; test_param.pCisParam[0].bnMToS = test_param.pCisParam[0].bnSToM = 1;
    test_param.pCisParam[0].phyMToS = test_param.pCisParam[0].phySToM = 1;
    test_param.pCisParam[0].pduSizeMToS = test_param.pCisParam[0].pduSizeSToM = 20;
    memset(&cis, 0, sizeof(cis)); cis.aclHandle = 1;
    ll_set_cis_test(&cis, &test_param, &test_param.pCisParam[0]); CHECK(debug_asserts == 0);
    test_param.pCisParam[0].nse = 1;
    ll_set_cis_test(&cis, &test_param, &test_param.pCisParam[0]); CHECK(debug_asserts == 1);

    /* Both PDU limits and publication order follow raw ll_set_cis: log SDU,
     * reload request sizes, cap with maxIsoPduLen, log PDU, then PHY calls. */
    reset_fixture(); le_cmd_unpack_cig_params(&param, command);
    log_tag_const_i_LL_CIS = 1; mutate_params = &param.pCisParam[0]; mutate_log = 1;
    ll_set_cis(&cis, &param, &param.pCisParam[0]);
    CHECK(cis.sduSizeMToS == 872 && cis.pduSizeMToS == 32 && cis.pduSizeSToM == 32);
    mutate_log = 0; log_tag_const_i_LL_CIS = 0;

    /* Two sequential streams: each CIG delay is the first stream's full
     * suffix duration, while each CIS has its own suffix duration. */
    reset_fixture(); memset(&param, 0, sizeof(param));
    param.cigId = 7; param.numCis = 2; param.sduIntervalMToS = param.sduIntervalSToM = 5000;
    param.transLatMToS = param.transLatSToM = 10;
    for (int i = 0; i < 2; i++) {
        param.pCisParam[i].cisId = i + 1;
        param.pCisParam[i].sduSizeMToS = param.pCisParam[i].sduSizeSToM = 100;
        param.pCisParam[i].phyMToS = param.pCisParam[i].phySToM = 1;
    }
    CHECK(ll_set_cig_params(&param) == 0 && debug_asserts == 0);
    CHECK(fixture_cis[0].cisSyncDelayUsec == 400 && fixture_cis[1].cisSyncDelayUsec == 200);
    CHECK(fixture_cis[0].cigSyncDelayUsec == 400 && fixture_cis[1].cigSyncDelayUsec == 400);
    CHECK(fixture_cig.cigSyncDelayUsec == 400 && fixture_cig.isoInterval == 4 && fixture_cig.isValid);
    enabled_cis = 1; available_cis = 0; CHECK(ll_set_cig_params(&param) == 7);
    enabled_cis = 0; CHECK(ll_set_cig_params(&param) == 9);

    /* Bad RSP posts failure and then unconditional success, both after
     * extended reject and stopping the response timer. High bytes are part
     * of the internal 32-bit offsets; they are not another wire decode. */
    reset_fixture(); active_cis = 1; fixture_cis[0].cisHandle = fixture_link.cisHandle = 101;
    fixture_cis[0].cig = &fixture_cig; fixture_cis[0].isCisReqPend = 1;
    u32 incoming_words[5] = {0}; struct cis_ll_msg *incoming = (void *)incoming_words;
    incoming->handle = fixture_link.handle; incoming->opcode = LL_PDU_CIS_RSP;
    *(cis_dword *)incoming->data = 0x01000001; *(cis_dword *)(incoming->data+4) = 2;
    ll_cis_master_msg_handler(LL_CIS_MST_MSG_RX_LLCP, incoming);
    CHECK(control_opcode == LL_REJECT_IND_EXT && control_args[0] == LL_PDU_CIS_RSP && control_args[1] == 13);
    CHECK(stops == 1 && sent_count == 2 && *(cis_word *)(messages[0]+2) == 0 && messages[0][6] == 13);
    CHECK(*(cis_word *)(messages[1]+2) == 1 && fixture_cis[0].llcp_state == 2);

    reset_fixture(); active_cis = 1; fixture_cis[0].cisHandle = fixture_link.cisHandle = 101;
    fixture_cis[0].cig = &fixture_cig; fixture_cig.isValid = fixture_cig.isBodBuilt = 1;
    u16 create_pair[2] = {1,101};
    ll_cis_master_msg_handler(LL_CIS_MST_MSG_CREATE_CIS, create_pair);
    CHECK(starts == 1 && control_opcode == LL_PDU_CIS_REQ && fixture_cis[0].ceRef == 19);
    CHECK(fixture_cig.isBodBuilt == 1 && fixture_cis[0].isCisReqPend == 1 && fixture_cis[0].llcp_state == 0);

    /* Completion logging can change pending count before the next scan. */
    reset_fixture(); active_cis = 1; fixture_cis[0].cisHandle = 101;
    lctrMstCreateCisPend.numCis = 2; lctrMstCreateCisPend.cisHandle[0] = 101;
    lctrMstCreateCisPend.cisHandle[1] = 102; ll_iso_hdl.createCisPend = 1;
    log_tag_const_i_LL_CIS = 1; mutate_done = 1;
    llcp_master_create_cis_done(&fixture_cis[0]);
    CHECK(sent_count == 0 && ll_iso_hdl.createCisPend == 0);
    mutate_done = 0;

    reset_fixture(); union {struct ble_rx rx; u8 bytes[sizeof(struct ble_rx)+12];} rsp_packet = {0};
    rsp_packet.bytes[offsetof(struct ble_rx, data)] = LL_PDU_CIS_RSP; mutate_rx = &rsp_packet.rx;
    ll_cis_master_send_rx_llcp_msg(&fixture_link, &rsp_packet.rx);
    CHECK(sent_count == 1 && messages[0][6] == 0xff && messages[0][8] == 0xaa);

    /* Invalid HCI production parameters still emit the complete payload
     * with the full opcode; source buffer is writable although typed const. */
    reset_fixture(); u8 invalid_command[32] = {0}; invalid_command[0] = 0xff;
    CHECK(le_cmd_set_cis_master(0x2062, invalid_command, 0) == 1);
    CHECK(hci_opcode == 0x2062 && hci_status == 48 && hci_size == 3);

    return TEST_DONE("btctrler_ll_cis_master");
}
#endif
