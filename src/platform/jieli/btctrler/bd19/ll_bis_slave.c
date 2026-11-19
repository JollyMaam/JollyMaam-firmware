/* BD19 broadcast isochronous slave lifecycle and LE BIG commands.  The
 * production and test-create commands share allocation and channel setup but
 * keep their distinct input layouts, validation, and timing arithmetic.
 * SDK quirks kept: CREATE BIG divides by an unchecked derived maxPdu, and the
 * completion event sends 36 + 2*numBis bytes from a fixed 36-byte event. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_BIS, log_tag_const_e_LL_BIS, log_tag_const_d_LL_BIS;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern int printf(const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))
typedef u16 bis_message_word __attribute__((may_alias));
extern u8 bis_emit_meta_event(u8, const char *, ...) __asm__("__hci_emit_le_meta_event");

static __attribute__((noinline)) void cpu_reset(void)
{
    p33_soft_reset();
}

static __inline__ __attribute__((always_inline)) void msg_alloc_assert(void *message, int line, int assert_policy)
{
    if (message)
        return;
    if (assert_policy) {
        printf("file:%s, line:%d",
               "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_bis_slave.c",
               line);
        printf("ASSERT-FAILD: msg ");
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
}

static __attribute__((noinline)) u8 lctrPhysBitToPhy(u8 phys_bit)
{
    return phys_bit == 4 ? 3 : (phys_bit == 2 ? 2 : 1);
}

static void ll_bis_slave_msg_handler(u8 msg_id, void *payload) LL_CODE;

static __inline__ __attribute__((always_inline)) u8 supported_phys_mask(void)
{
    u64 features = config_btctler_le_features;
    return (u8)((features & 0x800) ? 0xfc : ((((u32)features >> 7) & 2) ^ 0xfe));
}

static __inline__ __attribute__((always_inline)) int valid_big_handle(u8 handle)
{
    return handle <= 239;
}

static __inline__ __attribute__((always_inline)) int valid_num_bis(u8 num_bis)
{
    return (u8)(num_bis - 1) <= 30;
}

static __inline__ __attribute__((always_inline)) int valid_bn(u8 bn)
{
    return (u8)(bn - 1) <= 6;
}

static __inline__ __attribute__((always_inline)) int valid_sdu_interval(u32 interval)
{
    return interval - 256 <= 1048319;
}

static __inline__ __attribute__((always_inline)) void setup_encryption(
    le_big_t *big, const u8 *broadcast_code, int logging)
{
    u8 group_session_key[16];

    pseudo_random_genrate(big->giv, 8);
    pseudo_random_genrate(big->gskd, 16);
    lctrBisCalcGroupSessionKey(big->gskd, broadcast_code, group_session_key);
    if (logging) {
        log_print(2, NULL, "[Info]: [LL_BIS]BIG GSK :\r\n");
        printf_buf(group_session_key, 16);
    }
}

static __inline__ __attribute__((always_inline)) u32 setup_common(
    le_big_t *big,
    u8 num_bis,
    u16 max_sdu,
    u16 max_pdu,
    u32 sdu_interval,
    u32 iso_interval,
    u8 bn,
    u8 irc,
    u8 pto,
    u8 encrypt)
{
    big->maxPdu = max_pdu;
    big->maxSdu = max_sdu;
    big->sduInterUsec = sdu_interval;
    big->isoInterUsec = iso_interval;
    big->bn = bn;
    big->irc = irc;
    big->pto = pto;
    big->nse = (u8)((pto + bn) * irc);

    u32 shift;
    u32 base;
    switch (big->phy) {
    case 3:
        shift = 6;
        base = 870;
        break;
    case 2:
        shift = 2;
        base = 194;
        break;
    default:
        shift = 3;
        base = 230;
        break;
    }
    u32 packet_time = base + (((u8)max_pdu + (encrypt ? 4 : 0)) << shift);
    u32 burst_time = packet_time * big->nse;
    if (big->packing == 1) {
        big->subInterUsec = burst_time;
        big->bisSpaceUsec = packet_time;
    } else {
        big->subInterUsec = packet_time;
        big->bisSpaceUsec = burst_time;
    }
    big->syncDelayUsec = num_bis * burst_time;
    return burst_time;
}

static __inline__ __attribute__((always_inline)) u8 setup_bis_contexts(le_big_t *big, const u8 *num_bis)
{
    for (u32 i = 0; i < *num_bis; i++) {
        le_bis_t *bis = ll_bis_alloc(big);
        if (!bis) {
            ll_big_free(big);
            return 13;
        }
        ll_bis_setup(bis, big->seedAccAddr, big->baseCrcInit, 0xffffff, big->phy);
    }
    return 0;
}

static __inline__ __attribute__((always_inline)) void queue_create_big_message(
    le_big_t *big, int line)
{
    u8 *message = ll_iso_msg_alloc(2);
    msg_alloc_assert(message, line, config_asser);
    *(bis_message_word *)message = 10;
    *(bis_message_word *)(message + 2) = 0;
    *(bis_message_word *)(message + 4) = big->handle;
    ll_iso_msg_send(message);
}

static __inline__ __attribute__((always_inline)) u8 command_create_big_valid(
    u16 opcode, const u8 *data, int logging)
{
    (void)opcode;
    LlCreateBig_t command;
    __builtin_memset(&command, 0, sizeof(command));
    command.bigHandle = data[0];
    command.advHandle = data[1];
    command.numBis = data[2];
    command.sduInterUsec = data[3] | (data[4] << 8) | ((u32)data[5] << 16);
    command.maxSdu = data[6] | (data[7] << 8);
    command.mtlMs = data[8] | (data[9] << 8);
    command.rtn = data[10];
    command.phys = data[11];
    command.packing = data[12];
    command.framing = (LlFraming_t)data[13];
    command.encrypt = data[14];
    __builtin_memcpy(command.bcstCode, data + 15, sizeof(command.bcstCode));

    if (logging) {
        log_print(2, NULL, "[Info]: [LL_BIS]bigHandle     : 0x%x\r\n", command.bigHandle);
        log_print(2, NULL, "[Info]: [LL_BIS]advHandle     : 0x%x\r\n", command.advHandle);
        log_print(2, NULL, "[Info]: [LL_BIS]numBis        : 0x%x\r\n", command.numBis);
        log_print(2, NULL, "[Info]: [LL_BIS]sduInterUsec  : 0x%x\r\n", command.sduInterUsec);
        log_print(2, NULL, "[Info]: [LL_BIS]maxSdu        : 0x%x\r\n", command.maxSdu);
        log_print(2, NULL, "[Info]: [LL_BIS]mtlMs         : 0x%x\r\n", command.mtlMs);
        log_print(2, NULL, "[Info]: [LL_BIS]rtn           : 0x%x\r\n", command.rtn);
        log_print(2, NULL, "[Info]: [LL_BIS]phys          : 0x%x\r\n", command.phys);
        log_print(2, NULL, "[Info]: [LL_BIS]packing       : 0x%x\r\n", command.packing);
        log_print(2, NULL, "[Info]: [LL_BIS]framing       : 0x%x\r\n", command.framing);
        log_print(2, NULL, "[Info]: [LL_BIS]encrypt       : 0x%x\r\n", command.encrypt);
        log_print(2, NULL, "[Info]: [LL_BIS]bcstCode : \r\n");
        printf_buf(command.bcstCode, 16);
    }

    if (!valid_big_handle(command.bigHandle)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]bigHandle=%u out of range\r\n", command.bigHandle);
        goto invalid;
    }
    if (!valid_big_handle(command.advHandle)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]advHandle=%u out of range\r\n", command.advHandle);
        goto invalid;
    }
    if (!valid_num_bis(command.numBis)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]numBis=%u out of range\r\n", command.numBis);
        goto invalid;
    }
    if (!valid_sdu_interval(command.sduInterUsec)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]sduIntervUsec=%u out of range\r\n", command.sduInterUsec);
        goto invalid;
    }
    if (command.maxSdu > 4095) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]maxSdu=%u value out of range\r\n", command.maxSdu);
        goto invalid;
    }
    if (command.mtlMs > 4000) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]mtl=%u value out of range\r\n", command.mtlMs);
        goto invalid;
    }
    if (command.rtn > 15) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]rtn=%u value out of range\r\n", command.rtn);
        goto invalid;
    }
    if (command.phys == 0) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]phy=0x%02x value out of range\r\n", command.phys);
        goto invalid;
    }
    if (command.packing > 1) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]packing=%u value out of range\r\n", command.packing);
        goto invalid;
    }
    if (command.framing > 1) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]framing=%u value out of range\r\n", command.framing);
        goto invalid;
    }
    if (command.encrypt > 1) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]encryption=%u value out of range\r\n", command.encrypt);
        goto invalid;
    }
    if ((supported_phys_mask() & command.phys) != 0) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]Unknown PHY bits, phys=%u\r\n", command.phys);
        return 17;
    }
    if (command.numBis > ll_iso_hdl.max_bis_number) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]exceeds maximum allowed BIS context\r\n");
        return 13;
    }
    if (command.numBis > ll_get_number_avail_bis()) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]insufficient BIS context\r\n");
        return 13;
    }
    if (ll_big_by_handle(command.bigHandle)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]bigHandle=%u already in use\r\n",
                      command.bigHandle);
        return 12;
    }

    le_big_t *big = ll_big_alloc(command.bigHandle);
    if (!big) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]insufficient BIG context\r\n");
        return 13;
    }
    big->role = LL_ROLE_SLAVE;
    big->framing = command.framing;
    big->packing = (LlPacking_t)command.packing;
    big->phy = lctrPhysBitToPhy(ll_get_preferred_phy(command.phys));
    big->encrypt = command.encrypt;
    if (command.encrypt)
        setup_encryption(big, command.bcstCode, logging);
    u16 max_pdu = command.maxSdu < 251 ? command.maxSdu : 251;
    u8 bn = (u8)((command.maxSdu - 1 + max_pdu) / max_pdu);
    u32 burst_time = setup_common(
        big,
        command.numBis,
        command.maxSdu,
        max_pdu,
        command.sduInterUsec,
        command.sduInterUsec,
        bn,
        command.rtn + 1,
        0,
        command.encrypt);
    if (big->bn == 0 && command.sduInterUsec == 0) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]Invalid BN or sduInterUsec value\r\n");
        big->transLatUsec = 234;
    } else {
        big->transLatUsec = burst_time * command.numBis;
    }
    big->seedAccAddr = lctrComputeSeedAccessAddr();
    big->baseCrcInit = (u16)lctrComputeCrcInit();

    if ((big->maxSdu - 1 + big->maxPdu) / big->maxPdu > 8) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]exceeds maximum fragments max=%u\r\n", 8);
        return 18; /* SDK leaves the allocated BIG in place. */
    }
    u8 status = setup_bis_contexts(big, &command.numBis);
    if (status)
        return status;
    queue_create_big_message(big, 709);
    return 0;

invalid:
    if (log_tag_const_e_LL_BIS)
        log_print(4, NULL, "<Error>: [LL_BIS]PARAMETER_OUT_OF_MANDATORY_RANGE\r\n");
    return 48;
}

static __inline__ __attribute__((always_inline)) u8 command_create_big_test(
    u16 opcode, const u8 *data, int logging)
{
    (void)opcode;
    LlCreateBigTest_t command;
    __builtin_memset(&command, 0, sizeof(command));
    command.bigHandle = data[0];
    command.advHandle = data[1];
    command.numBis = data[2];
    command.sduInterUsec = data[3] | (data[4] << 8) | ((u32)data[5] << 16);
    command.isoInter = data[6] | (data[7] << 8);
    command.nse = data[8];
    command.maxSdu = data[9] | (data[10] << 8);
    command.maxPdu = data[11] | (data[12] << 8);
    command.phys = data[13];
    command.packing = data[14];
    command.framing = (LlFraming_t)data[15];
    command.bn = data[16];
    command.irc = data[17];
    command.pto = data[18];
    command.encrypt = data[19];
    __builtin_memcpy(command.bcstCode, data + 20, sizeof(command.bcstCode));

    if (logging) {
        log_print(2, NULL, "[Info]: [LL_BIS]bigHandle     : 0x%x\r\n", command.bigHandle);
        log_print(2, NULL, "[Info]: [LL_BIS]advHandle     : 0x%x\r\n", command.advHandle);
        log_print(2, NULL, "[Info]: [LL_BIS]numBis        : 0x%x\r\n", command.numBis);
        log_print(2, NULL, "[Info]: [LL_BIS]sduInterUsec  : 0x%x\r\n", command.sduInterUsec);
        log_print(2, NULL, "[Info]: [LL_BIS]isoInter      : 0x%x\r\n", command.isoInter);
        log_print(2, NULL, "[Info]: [LL_BIS]nse           : 0x%x\r\n", command.nse);
        log_print(2, NULL, "[Info]: [LL_BIS]maxSdu        : 0x%x\r\n", command.maxSdu);
        log_print(2, NULL, "[Info]: [LL_BIS]maxPdu        : 0x%x\r\n", command.maxPdu);
        log_print(2, NULL, "[Info]: [LL_BIS]phys          : 0x%x\r\n", command.phys);
        log_print(2, NULL, "[Info]: [LL_BIS]packing       : 0x%x\r\n", command.packing);
        log_print(2, NULL, "[Info]: [LL_BIS]framing       : 0x%x\r\n", command.framing);
        log_print(2, NULL, "[Info]: [LL_BIS]bn            : 0x%x\r\n", command.bn);
        log_print(2, NULL, "[Info]: [LL_BIS]irc           : 0x%x\r\n", command.irc);
        log_print(2, NULL, "[Info]: [LL_BIS]pto           : 0x%x\r\n", command.pto);
        log_print(2, NULL, "[Info]: [LL_BIS]encrypt       : 0x%x\r\n", command.encrypt);
        log_print(2, NULL, "[Info]: [LL_BIS]bcstCode : \r\n");
        printf_buf(command.bcstCode, 16);
    }

    if (!valid_big_handle(command.bigHandle)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]bigHandle=%u out of range\r\n", command.bigHandle);
        goto invalid;
    }
    if (!valid_big_handle(command.advHandle)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]advHandle=%u out of range\r\n", command.advHandle);
        goto invalid;
    }
    if (!valid_num_bis(command.numBis)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]numBis=%u out of range\r\n", command.numBis);
        goto invalid;
    }
    if (!valid_sdu_interval(command.sduInterUsec)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]sduInterval=%u out of range\r\n", command.sduInterUsec);
        goto invalid;
    }
    if ((u16)(command.isoInter - 4) > 3196) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]isoInterval=%u out of range\r\n", command.isoInter);
        goto invalid;
    }
    if (!valid_num_bis(command.nse)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]NSE=%u out of range\r\n", command.nse);
        goto invalid;
    }
    if (command.maxSdu > 4095) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]maxSdu=%u out of range\r\n", command.maxSdu);
        goto invalid;
    }
    if ((u16)(command.maxPdu - 1) > 250) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]maxPdu=%u out of range\r\n", command.maxPdu);
        goto invalid;
    }
    if (command.phys == 0) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]phy=0x%02x out of range\r\n", command.phys);
        goto invalid;
    }
    if (command.packing > 1) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]packing=%u out of range\r\n", command.packing);
        goto invalid;
    }
    if (command.framing > 1) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]framing=%u out of range\r\n", command.framing);
        goto invalid;
    }
    if (!valid_bn(command.bn)) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]BN=%u out of range\r\n", command.bn);
        goto invalid;
    }
    if ((u8)(command.irc - 1) > 14) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]IRC=%u out of range\r\n", command.irc);
        goto invalid;
    }
    if (command.pto > 15) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]PTO=%u out of range\r\n", command.pto);
        goto invalid;
    }
    if (command.encrypt > 1) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]encrypt=%u out of range\r\n", command.encrypt);
        goto invalid;
    }
    if (command.framing == 0 &&
        ((u32)command.isoInter * 1250) % command.sduInterUsec != 0) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL,
                      "<Error>: [LL_BIS]Unframed PDUs must have ISO_Interval that are multiples of the SDU_Interval\r\n");
        goto invalid;
    }
    if (command.nse < command.irc * command.bn) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]NSE not less than 'IRC * BN'\r\n");
        return 17;
    }
    if ((supported_phys_mask() & command.phys) != 0) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]Unknown PHY bits, phys=%u\r\n", command.phys);
        return 17;
    }
    if (command.numBis > ll_get_number_avail_bis()) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]insufficient BIS context\r\n");
        return 13;
    }
    if (ll_big_by_handle(command.bigHandle)) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]bigHandle=%u already in use\r\n",
                      command.bigHandle);
        return 12;
    }

    le_big_t *big = ll_big_alloc(command.bigHandle);
    if (!big) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]insufficient BIG context\r\n");
        return 13;
    }
    big->role = LL_ROLE_SLAVE;
    big->framing = command.framing;
    big->packing = (LlPacking_t)command.packing;
    big->phy = lctrPhysBitToPhy(ll_get_preferred_phy(command.phys));
    big->encrypt = command.encrypt;
    if (command.encrypt)
        setup_encryption(big, command.bcstCode, logging);
    u32 iso_interval = (u32)command.isoInter * 1250;
    u32 burst_time = setup_common(
        big, command.numBis, command.maxSdu, command.maxPdu,
        command.sduInterUsec, iso_interval,
        command.bn, command.irc, command.pto, command.encrypt);
    if (command.sduInterUsec == 0 && command.bn == 0) {
        if (log_tag_const_e_LL_BIS)
            log_print(4, NULL, "<Error>: [LL_BIS]Invalid BN or sduInterUsec value\r\n");
        big->transLatUsec = 234;
    } else {
        big->transLatUsec =
            ((u32)command.pto * iso_interval) *
                ((big->nse / command.bn) - command.irc) +
            burst_time * command.numBis +
            ((iso_interval / command.sduInterUsec) - 1) * command.sduInterUsec;
    }
    big->seedAccAddr = lctrComputeSeedAccessAddr();
    big->baseCrcInit = (u16)lctrComputeCrcInit();

    if (((big->maxSdu - 1 + big->maxPdu) / big->maxPdu) > 8) {
        if (log_tag_const_d_LL_BIS)
            log_print(1, NULL, "[Debug]: [LL_BIS]exceeds maximum fragments max=%u\r\n", 8);
        return 18; /* SDK leaves the allocated BIG in place. */
    }
    u8 status = setup_bis_contexts(big, &command.numBis);
    if (status)
        return status;
    queue_create_big_message(big, 782);
    return 0;

invalid:
    if (log_tag_const_e_LL_BIS)
        log_print(4, NULL, "<Error>: [LL_BIS]PARAMETER_OUT_OF_MANDATORY_RANGE\r\n");
    return 48;
}

void llcp_slv_create_big(u16 handle) LL_CODE;
void llcp_slv_create_big(u16 handle)
{
    le_big_t *big = ll_big_by_handle(handle);
    LlCreateBigCnf_t event;

    event.status = 0;
    event.bigHandle = big->handle;
    event.syncDelayUsec = big->syncDelayUsec;
    event.transLatUsec = big->transLatUsec;
    event.phy = big->phy;
    event.nse = big->nse;
    event.bn = big->bn;
    event.pto = big->pto;
    event.irc = big->irc;
    event.maxPdu = big->maxPdu;
    event.isoInterval = (u16)(big->isoInterUsec / 1250);
    event.numBis = big->numBis;

    /* The SDK extends its fixed struct through the BIS-handle tail. */
    u8 size = 36;
    int index = 0;
    le_bis_t *bis = (le_bis_t *)big->bis_head.next;
    while (&bis->entry != &big->bis_head) {
        event.bisHandle[index++] = bis->handle;
        size = (u8)(size + 2);
        bis = (le_bis_t *)bis->entry.next;
    }
    int assert_policy = config_asser;
    if (index >= 7) {
        if (assert_policy) {
            printf("file:%s, line:%d",
                   "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_bis_slave.c",
                   144);
            printf("ASSERT-FAILD: i <= LL_MAX_BIS ");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    (void)hci_send_event_payload(27, (u8 *)&event, size);

    u8 *message = ll_msg_alloc(2);
    msg_alloc_assert(message, 197, assert_policy);
    *(bis_message_word *)message = 8;
    *(bis_message_word *)(message + 2) = 0;
    *(bis_message_word *)(message + 4) = big->handle;
    ll_msg_send(message);
}

void llcp_slv_terminated_big(u16 handle, u8 reason) LL_CODE;
void llcp_slv_terminated_big(u16 handle, u8 reason)
{
    le_big_t *big = ll_big_by_handle(handle);
    (void)bis_emit_meta_event(28, "11", big->handle, reason);
    ll_big_free(big);
}

int le_cmd_set_bis_slave(u16 opcode, const u8 *data, u32 size) LL_CODE;
int le_cmd_set_bis_slave(u16 opcode, const u8 *data, u32 size)
{
    (void)size;
    u32 full_opcode = opcode;
    switch (opcode & 0x03ffu) {
    case 104: {
        int logging = log_tag_const_i_LL_BIS != 0;
        if (logging)
            log_print(2, NULL, "[Info]: [LL_BIS]HCI_LE_CREATE_BIG\r\n");
        u8 status = command_create_big_valid(opcode, data, logging);
        (void)hci_send_event(15, "112", status, 1, full_opcode);
        return 1;
    }
    case 105: {
        int logging = log_tag_const_i_LL_BIS != 0;
        if (logging)
            log_print(2, NULL, "[Info]: [LL_BIS]HCI_LE_CREATE_BIG_TEST\r\n");
        u8 status = command_create_big_test(opcode, data, logging);
        (void)hci_send_event(15, "112", status, 1, full_opcode);
        return 1;
    }
    case 106:
        if (log_tag_const_i_LL_BIS)
            log_print(2, NULL, "[Info]: [LL_BIS]HCI_LE_TERMINATE_BIG\r\n");
        return 1;
    default:
        return 0;
    }
}

void ll_bis_slave_init(void) LL_CODE;
void ll_bis_slave_init(void)
{
    ll_dispatch_msg_table[10] = ll_bis_slave_msg_handler;
}

static void ll_bis_slave_msg_handler(u8 msg_id, void *payload) LL_CODE;
static void ll_bis_slave_msg_handler(u8 msg_id, void *payload)
{
    switch (msg_id) {
    case 0:
        if (log_tag_const_i_LL_BIS)
            log_print(2, NULL, "[Info]: [LL_BIS]LL_BIS_SLV_MSG_CREATE_BIG\r\n");
        llcp_slv_create_big(*(bis_message_word *)payload);
        break;
    case 1:
        if (log_tag_const_i_LL_BIS)
            log_print(2, NULL, "[Info]: [LL_BIS]LL_BIS_SLV_MSG_TERMINATED_BIG\r\n");
        llcp_slv_terminated_big(*(bis_message_word *)payload, *((u8 *)payload + 2));
        break;
    default:
        break;
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <setjmp.h>

u8 log_tag_const_i_LL_BIS, log_tag_const_e_LL_BIS, log_tag_const_d_LL_BIS;
u64 config_btctler_le_features;
int config_asser;
ll_iso_handle_t ll_iso_hdl;
ll_dispatch_msg_t ll_dispatch_msg_table[15];

static int logs_i, logs_e, logs_d, printf_buf_len;
static int big_alloc_calls, big_free_calls, bis_alloc_calls, bis_setup_calls;
static int iso_alloc_calls, iso_send_calls, ll_alloc_calls, ll_send_calls;
static int send_event_calls, send_event_status, send_event_num, send_event_opcode;
static int event_payload_calls, event_payload_code, event_payload_size;
static u8 event_payload[64];
static int meta_event_calls, meta_event_code, meta_event_first, meta_event_reason;
static le_big_t big_storage;
static le_big_t *current_big;
static le_bis_t bis_storage[8];
static int bis_count;
static u8 iso_message[8], ll_message[8];
static int resets, asserts;
static u32 seed_value, crc_value;
static u8 setup_phy;
static u64 setup_chmap;
static u16 setup_crc;
static u32 setup_seed;
static int fail_big_alloc, fail_bis_alloc_at, mutate_encryption, mutate_seed;
static int preferred_override;
static int fail_message_alloc, mutate_event_policy, trap_failure;
static jmp_buf failure_jump;
static char callback_trace[64];
static void record_callback(char c)
{
    size_t n = strlen(callback_trace);
    CHECK(n + 1 < sizeof(callback_trace));
    if (n + 1 < sizeof(callback_trace)) { callback_trace[n] = c; callback_trace[n + 1] = 0; }
}

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
    if (level == 4)
        logs_e++;
    else if (level == 1)
        logs_d++;
    else
        logs_i++;
}

void printf_buf(const void *buffer, int size)
{
    CHECK(buffer != NULL);
    printf_buf_len = size;
}

void p33_soft_reset(void) { resets++; if (trap_failure) longjmp(failure_jump, 1); }
void cpu_assert_debug(void) { asserts++; if (trap_failure) longjmp(failure_jump, 1); }

le_big_t *ll_big_by_handle(u16 handle)
{
    return current_big && current_big->handle == handle ? current_big : NULL;
}

le_big_t *ll_big_alloc(u8 handle)
{
    big_alloc_calls++;
    record_callback('A');
    if (fail_big_alloc) return NULL;
    memset(&big_storage, 0, sizeof(big_storage));
    big_storage.handle = handle;
    big_storage.bis_head.next = &big_storage.bis_head;
    big_storage.bis_head.prev = &big_storage.bis_head;
    current_big = &big_storage;
    return current_big;
}

void ll_big_free(le_big_t *big)
{
    CHECK(big == current_big);
    big_free_calls++;
    current_big = NULL;
}

u8 ll_get_number_avail_bis(void) { return 6; }

le_bis_t *ll_bis_alloc(le_big_t *big)
{
    CHECK(big == current_big);
    bis_alloc_calls++;
    record_callback('B');
    if (fail_bis_alloc_at == bis_alloc_calls) return NULL;
    le_bis_t *bis = &bis_storage[bis_count];
    memset(bis, 0, sizeof(*bis));
    bis->handle = (u16)(100 + bis_count);
    bis->big = big;
    struct list_head *head = &big->bis_head;
    struct list_head *previous = head->prev;
    bis->entry.prev = previous;
    bis->entry.next = head;
    previous->next = &bis->entry;
    head->prev = &bis->entry;
    bis_count++;
    big->numBis++;
    return bis;
}

void ll_bis_setup(le_bis_t *bis, uint32_t seed_acc_addr, uint16_t base_crc_init,
                  uint64_t ch_map, LlPhy_t phy)
{
    CHECK(bis == &bis_storage[bis_setup_calls]);
    bis_setup_calls++;
    record_callback('S');
    setup_seed = seed_acc_addr;
    setup_crc = base_crc_init;
    setup_chmap = ch_map;
    setup_phy = (u8)phy;
}

void *ll_iso_msg_alloc(u16 size)
{
    CHECK(size == 2);
    iso_alloc_calls++;
    memset(iso_message, 0, sizeof(iso_message));
    return iso_message;
}

void ll_iso_msg_send(void *message)
{
    CHECK(message == iso_message);
    iso_send_calls++;
    record_callback('Q');
}

void *ll_msg_alloc(u16 size)
{
    CHECK(size == 2);
    ll_alloc_calls++;
    if (fail_message_alloc) return NULL;
    memset(ll_message, 0, sizeof(ll_message));
    return ll_message;
}

void ll_msg_send(void *message)
{
    CHECK(message == ll_message);
    ll_send_calls++;
}

int hci_send_event(int opcode, const char *format, ...)
{
    va_list args;
    CHECK(opcode == 15 && !strcmp(format, "112"));
    va_start(args, format);
    send_event_status = va_arg(args, int);
    send_event_num = va_arg(args, int);
    send_event_opcode = va_arg(args, int);
    va_end(args);
    send_event_calls++;
    record_callback('E');
    return 0;
}

int hci_send_event_payload(int code, u8 *payload, u8 size)
{
    event_payload_calls++;
    if (mutate_event_policy) config_asser = 0;
    event_payload_code = code;
    event_payload_size = size;
    /* The production SDK over-reads its fixed event for the second BIS. */
    size_t copy_size = size;
    if (copy_size > sizeof(LlCreateBigCnf_t))
        copy_size = sizeof(LlCreateBigCnf_t);
    memcpy(event_payload, payload, copy_size);
    return 0;
}

u8 bis_emit_meta_event(u8 subevent_code, const char *format, ...)
{
    va_list args;
    CHECK(!strcmp(format, "11"));
    va_start(args, format);
    meta_event_first = va_arg(args, int);
    meta_event_reason = va_arg(args, int);
    va_end(args);
    meta_event_calls++;
    meta_event_code = subevent_code;
    return true;
}

u8 ll_get_preferred_phy(u8 phys) { record_callback('P'); return preferred_override ? preferred_override : phys; }
void pseudo_random_genrate(u8 *dest, unsigned int size) { memset(dest, 0x5a, size); }
void lctrBisCalcGroupSessionKey(const u8 *gskd, const u8 *bc, u8 *gsk)
{
    CHECK(gskd && bc && gsk);
    record_callback('K');
    if (mutate_encryption) {
        LlCreateBigTest_t *command = (LlCreateBigTest_t *)((u8 *)bc - offsetof(LlCreateBigTest_t, bcstCode));
        command->maxPdu = 300; command->numBis = 2;
        current_big->phy = 2; current_big->packing = 2;
    }
    memset(gsk, 0xa5, 16);
}
u32 lctrComputeSeedAccessAddr(void)
{
    record_callback('R');
    if (mutate_seed) {
        current_big->maxSdu = mutate_seed == 1 ? 2048 : 0;
        current_big->maxPdu = 1;
    }
    return seed_value;
}
u32 lctrComputeCrcInit(void) { return crc_value; }

static void fill_create_big(u8 *data)
{
    memset(data, 0, 31);
    data[0] = 3;
    data[1] = 4;
    data[2] = 2;
    data[3] = 0xe8;
    data[4] = 0x03;
    data[5] = 0;
    data[6] = 200;
    data[7] = 0;
    data[8] = 0xe8;
    data[9] = 0x03;
    data[10] = 1;
    data[11] = 1;
}

static void fill_create_big_test(u8 *data)
{
    memset(data, 0, 36);
    data[0] = 5;
    data[1] = 6;
    data[2] = 1;
    data[3] = 0xe8;
    data[4] = 0x03;
    data[5] = 0;
    data[6] = 4;
    data[7] = 0;
    data[8] = 2;
    data[9] = 100;
    data[10] = 0;
    data[11] = 100;
    data[12] = 0;
    data[13] = 1;
    data[14] = 1;
    data[15] = 0;
    data[16] = 1;
    data[17] = 1;
    data[18] = 1;
    data[19] = 0;
}

int main(void)
{
    u8 command[40];
    u8 message[4] = {0};
    int allocs;
    seed_value = 0x11223344;
    crc_value = 0x5566;
    log_tag_const_i_LL_BIS = log_tag_const_e_LL_BIS = log_tag_const_d_LL_BIS = 0;
    ll_iso_hdl.max_bis_number = 6;

    ll_bis_slave_init();
    CHECK(ll_dispatch_msg_table[10] == ll_bis_slave_msg_handler);

    CHECK(lctrPhysBitToPhy(4) == 3 && lctrPhysBitToPhy(2) == 2 &&
          lctrPhysBitToPhy(1) == 1 && lctrPhysBitToPhy(0) == 1);

    /* CREATE BIG: two BIS, unframed, PHY 1.  IR lines 224-352 derive BN,
     * NSE, packet time, sub-interval and sync delay from the parsed fields. */
    fill_create_big(command);
    CHECK(le_cmd_set_bis_slave(104, command, 31) == 1);
    CHECK(send_event_calls == 1 && send_event_status == 0 &&
          send_event_num == 1 && send_event_opcode == 104);
    CHECK(big_alloc_calls == 1 && bis_alloc_calls == 2 && bis_setup_calls == 2);
    CHECK(current_big->role == LL_ROLE_SLAVE && current_big->maxPdu == 200 &&
          current_big->maxSdu == 200 && current_big->bn == 1 &&
          current_big->irc == 2 && current_big->pto == 0 && current_big->nse == 2);
    CHECK(current_big->sduInterUsec == 1000 && current_big->isoInterUsec == 1000);
    CHECK(current_big->subInterUsec == 1830 && current_big->bisSpaceUsec == 3660 &&
          current_big->syncDelayUsec == 7320 && current_big->transLatUsec == 7320);
    CHECK(current_big->seedAccAddr == 0x11223344 && current_big->baseCrcInit == 0x5566);
    CHECK(setup_chmap == 0xffffff && setup_phy == 1);
    CHECK(iso_alloc_calls == 1 && iso_send_calls == 1);
    CHECK(*(u16 *)iso_message == 10 && *(u16 *)(iso_message + 2) == 0 &&
          *(u16 *)(iso_message + 4) == 3);

    /* The queued create message emits the completion event and queues the
     * state-8 LL message carrying the BIG handle. */
    message[0] = 3;
    ll_dispatch_msg_table[10](0, message);
    CHECK(event_payload_calls == 1 && event_payload_code == 27 && event_payload_size == 40);
    LlCreateBigCnf_t *event = (LlCreateBigCnf_t *)event_payload;
    CHECK(event->status == 0 && event->bigHandle == 3 && event->syncDelayUsec == 7320 &&
          event->transLatUsec == 7320 && event->nse == 2 && event->bn == 1 &&
          event->irc == 2 && event->maxPdu == 200 && event->isoInterval == 0 &&
          event->numBis == 2 && event->bisHandle[0] == 100 && event->bisHandle[1] == 101);
    CHECK(ll_alloc_calls == 1 && ll_send_calls == 1 &&
          *(u16 *)ll_message == 8 && *(u16 *)(ll_message + 4) == 3);

    /* Terminate message emits LE BIG Terminated and frees the BIG. */
    message[0] = 3;
    message[2] = 0x24;
    ll_dispatch_msg_table[10](1, message);
    CHECK(meta_event_calls == 1 && meta_event_code == 28 &&
          meta_event_first == 3 && meta_event_reason == 0x24);
    CHECK(big_free_calls == 1 && current_big == NULL);

    /* CREATE BIG TEST: its ISR/ISO interval arithmetic is distinct from
     * CREATE BIG.  With PTO 1, BN 1, IRC 1 and interval 5000 us, IR line 575
     * gives translation latency 11060 us. */
    callback_trace[0] = 0;
    fill_create_big_test(command);
    CHECK(le_cmd_set_bis_slave(105, command, 36) == 1);
    CHECK(send_event_calls == 2 && send_event_status == 0 && send_event_opcode == 105);
    CHECK(current_big->nse == 2 && current_big->bn == 1 && current_big->irc == 1 &&
          current_big->pto == 1 && current_big->isoInterUsec == 5000);
    CHECK(current_big->subInterUsec == 2060 && current_big->bisSpaceUsec == 1030 &&
          current_big->syncDelayUsec == 2060 && current_big->transLatUsec == 11060);

    fill_create_big_test(command);
    log_tag_const_e_LL_BIS = 1;
    allocs = big_alloc_calls;
    int errors = logs_e;
    CHECK(le_cmd_set_bis_slave(105, command, 36) == 1);
    CHECK(send_event_status == 12 && big_alloc_calls == allocs && logs_e == errors + 1);
    log_tag_const_e_LL_BIS = 0;

    fill_create_big_test(command);
    command[16] = 8;                         /* BN is limited to 1..7 */
    allocs = big_alloc_calls;
    CHECK(le_cmd_set_bis_slave(105, command, 36) == 1);
    CHECK(send_event_status == 48 && big_alloc_calls == allocs);

    /* Invalid numBis fails mandatory-range validation before allocation. */
    fill_create_big(command);
    command[2] = 0;
    CHECK(le_cmd_set_bis_slave(104, command, 31) == 1);
    CHECK(send_event_status == 48 && big_alloc_calls == allocs);

    /* TERMINATE BIG is handled without emitting a command-complete event. */
    int events = send_event_calls;
    CHECK(le_cmd_set_bis_slave(106, NULL, 0) == 1);
    CHECK(send_event_calls == events);
    CHECK(le_cmd_set_bis_slave(103, NULL, 0) == 0);

    /* Independent raw PHY switch vectors: PHY 2 -> 194 + (100 << 2),
     * PHY 3 -> 870 + (100 << 6); each uses two subevents. */
    for (int phy = 2; phy <= 3; phy++) {
        current_big = NULL; bis_count = bis_setup_calls = bis_alloc_calls = 0;
        callback_trace[0] = 0; preferred_override = phy == 3 ? 4 : 2;
        config_btctler_le_features = 0x100;
        fill_create_big_test(command); command[13] = 2;
        CHECK(le_cmd_set_bis_slave(0x2069, command, 0) == 1);
        CHECK(send_event_status == 0 && send_event_opcode == 0x2069);
        CHECK(current_big->subInterUsec == (phy == 2 ? 1188 : 14540));
        CHECK(current_big->bisSpaceUsec == (phy == 2 ? 594 : 7270));
        CHECK(!strcmp(callback_trace, "APRBSQE"));
        CHECK(current_big->numBis == 1); /* allocator increments, caller does not */
    }
    preferred_override = 0; config_btctler_le_features = 0;

    /* The session-key callback can reach the decoded command through bcstCode.
     * Raw IR reloads both that command and BIG's PHY/packing afterwards. */
    current_big = NULL; bis_count = bis_setup_calls = bis_alloc_calls = 0;
    callback_trace[0] = 0; fill_create_big_test(command); command[19] = 1;
    mutate_encryption = 1;
    CHECK(le_cmd_set_bis_slave(105, command, 36) == 1 && send_event_status == 0);
    CHECK(current_big->maxPdu == 300 && current_big->phy == 2 && current_big->packing == 2);
    CHECK(current_big->subInterUsec == 386 && current_big->bisSpaceUsec == 772);
    CHECK(current_big->syncDelayUsec == 1544 && current_big->transLatUsec == 10544);
    CHECK(current_big->numBis == 2 && !strcmp(callback_trace, "APKRBSBSQE"));
    mutate_encryption = 0;

    /* Fragment rejection reads the BIG again after seed/CRC callbacks and
     * leaves it allocated. BIS allocation failure instead frees the BIG. */
    current_big = NULL; bis_count = bis_setup_calls = bis_alloc_calls = 0;
    callback_trace[0] = 0; fill_create_big_test(command); mutate_seed = 1;
    int frees = big_free_calls;
    CHECK(le_cmd_set_bis_slave(105, command, 36) == 1 && send_event_status == 18);
    CHECK(current_big != NULL && big_free_calls == frees && bis_alloc_calls == 0);
    current_big = NULL; callback_trace[0] = 0; mutate_seed = 2;
    CHECK(le_cmd_set_bis_slave(105, command, 36) == 1 && send_event_status == 0);
    CHECK(current_big->maxSdu == 0 && bis_alloc_calls == 1);
    mutate_seed = 0;
    current_big = NULL; bis_count = bis_setup_calls = bis_alloc_calls = 0;
    callback_trace[0] = 0; fill_create_big(command); fail_bis_alloc_at = 2;
    CHECK(le_cmd_set_bis_slave(104, command, 31) == 1 && send_event_status == 13);
    CHECK(current_big == NULL && big_free_calls == frees + 1 && iso_send_calls > 0);
    fail_bis_alloc_at = 0; fail_big_alloc = 1; callback_trace[0] = 0;
    CHECK(le_cmd_set_bis_slave(104, command, 31) == 1 && send_event_status == 13);
    CHECK(!strcmp(callback_trace, "AE"));
    fail_big_alloc = 0;

    /* Assertion policy is captured before the event and retained across the
     * subsequent allocation. Escape at the selected failure hook so the test
     * does not execute the SDK's following null message stores. */
    current_big = &big_storage; big_storage.handle = 5;
    big_storage.bis_head.next = big_storage.bis_head.prev = &big_storage.bis_head;
    config_asser = 1; mutate_event_policy = fail_message_alloc = trap_failure = 1;
    if (setjmp(failure_jump) == 0) {
        llcp_slv_create_big(5);
        CHECK(0);
    }
    CHECK(asserts == 1 && resets == 0 && config_asser == 0);
    trap_failure = fail_message_alloc = mutate_event_policy = 0;

    return TEST_DONE("btctrler_ll_bis_slave");
}
#endif
