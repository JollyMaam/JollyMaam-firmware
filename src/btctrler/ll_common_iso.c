/* Isochronous (LE Audio) common layer: lifecycle, HCI ISO commands and data,
 * ISOAL fragmentation/recombination/reassembly, and the SDK's ISOAL self
 * test.  SDK quirks kept: the CIS fragmentation writes its PDU through a null
 * pdu pointer (the SDK never assigned it); hci_push_iso_sdu logs "ISO SDU : %d"
 * with no argument; unpack leaves the SDU length and packet status
 * uninitialised for continuation fragments and clears the timestamp for
 * first-fragment and complete SDUs; lctrIsoSendCodecSdu leaks its buffer when
 * the codec returns data; the reassembler retains its completion buffer and
 * writes an unchecked framed length before packing the host header. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_ISO, log_tag_const_e_LL_ISO, log_tag_const_d_LL_ISO;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern u32 sys_timer_get_ms(void);
extern int printf(const char *format, ...);
#ifdef UNITTEST
#include <stdlib.h>
void *ll_iso_test_malloc(u32 size);
void ll_iso_test_free(void *ptr);
#define malloc ll_iso_test_malloc
#define free ll_iso_test_free
#else
extern void *malloc(u32 size);
extern void free(void *ptr);
#endif

#define LL_CODE __attribute__((section(".ble_ll_code")))
typedef u32 iso_wire_u32 __attribute__((aligned(1), may_alias));
typedef u32 iso_hdr_word __attribute__((may_alias));
typedef u64 iso_wire_u64 __attribute__((aligned(1), may_alias));

#ifdef UNITTEST
extern u8 *ll_iso_null_pdu;           /* stands in for address 0 */
#define NULL_PDU ll_iso_null_pdu
#else
#define NULL_PDU ((u8 *)0)
#endif

#ifndef UNITTEST
_Static_assert(sizeof(hci_iso_hdr_t) == 16, "SDK ISO header size");
_Static_assert(sizeof(ll_iso_rt_cfg_t) == 14, "SDK ISO runtime config size");
_Static_assert(offsetof(ll_iso_rt_cfg_t, maxIsoSduLen) == 2, "SDK maxIsoSduLen offset");
_Static_assert(sizeof(le_cis_t) == 356, "SDK CIS size");
_Static_assert(offsetof(le_cis_t, role) == 24, "SDK CIS role offset");
_Static_assert(offsetof(le_cis_t, framing) == 200, "SDK CIS framing offset");
_Static_assert(offsetof(le_cis_t, pduSizeMToS) == 218, "SDK CIS pdu size offset");
_Static_assert(offsetof(le_cis_t, bnMToS) == 232, "SDK CIS bnMToS offset");
_Static_assert(offsetof(le_cis_t, bnSToM) == 233, "SDK CIS bnSToM offset");
_Static_assert(offsetof(le_bis_t, big) == 12, "SDK BIS big offset");
_Static_assert(offsetof(le_big_t, maxSdu) == 22, "SDK BIG maxSdu offset");
#endif

ll_iso_rt_cfg_t ll_iso_rt_cfg __attribute__((section(".ble_ll_data"), aligned(2)));
ll_codec_t ll_codec __attribute__((section(".ble_ll_bss"), aligned(4)));
ll_iso_handle_t ll_iso_hdl __attribute__((section(".ble_ll_bss"), aligned(4)));
ll_dispatch_msg_t ll_dispatch_msg_table[15] __attribute__((section(".ble_ll_bss"), aligned(4)));
static u32 ll_iso_process_time_water_mark __attribute__((section(".ble_ll_data"), aligned(4))) = -1;

static void cpu_reset(void)
{
    p33_soft_reset();
}

static __inline__ __attribute__((always_inline)) void isoal_alloc_assert(u8 *buffer, u32 size)
{
    if (buffer)
        return;
    if (config_asser) {
        printf("file:%s, line:%d",
               "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/BLE/ll_common_iso.c",
               1302);
        printf("ASSERT-FAILD: pRxCtx->pPendSduBuf Framed SDU alloc failed");
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
    (void)size;
}

static __inline__ __attribute__((always_inline)) u32 iso_hdr_first_word(const hci_iso_hdr_t *hdr)
{
    u32 word;
    __builtin_memcpy(&word, hdr, sizeof(word));
    return word;
}

static __inline__ __attribute__((always_inline)) void iso_hdr_set_first_word(hci_iso_hdr_t *hdr, u32 word)
{
    __builtin_memcpy(hdr, &word, sizeof(word));
}

static __inline__ __attribute__((always_inline)) u32 iso_hdr_second_word(const hci_iso_hdr_t *hdr)
{
    return *(const iso_hdr_word *)((const u8 *)hdr + 8);
}

static __inline__ __attribute__((always_inline)) void iso_hdr_set_second_word(hci_iso_hdr_t *hdr, u32 word)
{
    *(iso_hdr_word *)((u8 *)hdr + 8) = word;
}

u8 ll_iso_pack_hdr(u8 *sdu, const hci_iso_hdr_t *hdr) LL_CODE;
u8 ll_iso_pack_hdr(u8 *sdu, const hci_iso_hdr_t *hdr)
{
    u32 first = iso_hdr_first_word(hdr);
    u8 offset = 4;
    sdu[0] = (u8)first;
    sdu[1] = (u8)((first >> 8) & 0x7f);
    u8 logging = log_tag_const_i_LL_ISO;
    if (logging) {
        log_print(2, NULL, "[Info]: [LL_ISO]-----Pack\r\n");
        log_print(2, NULL, "[Info]: [LL_ISO]Handle    : 0x%x\r\n", iso_hdr_first_word(hdr) & 0xfff);
        log_print(2, NULL, "[Info]: [LL_ISO]PB Flag   : 0x%x\r\n", (iso_hdr_first_word(hdr) >> 12) & 3);
        log_print(2, NULL, "[Info]: [LL_ISO]TS Flag   : 0x%x\r\n", (iso_hdr_first_word(hdr) >> 14) & 1);
    }
    u8 *data = sdu + 4;
    if (iso_hdr_first_word(hdr) & 0x4000) {
        data[0] = (u8)hdr->time_stamp;
        data[1] = (u8)(hdr->time_stamp >> 8);
        data[2] = (u8)(hdr->time_stamp >> 16);
        data[3] = (u8)(hdr->time_stamp >> 24);
        data += 4; offset = 8;
        if (logging)
            log_print(2, NULL, "[Info]: [LL_ISO]+[1]TS               : 0x%x\r\n", hdr->time_stamp);
    }
    u16 boundary = (u16)((iso_hdr_first_word(hdr) >> 12) & 3);
    if (boundary == 0 || boundary == 2) {
        data[0] = (u8)iso_hdr_second_word(hdr);
        data[1] = (u8)(iso_hdr_second_word(hdr) >> 8);
        data[2] = (u8)(iso_hdr_second_word(hdr) >> 16);
        data[3] = (u8)(iso_hdr_second_word(hdr) >> 24) & 0x0f;
        offset = (u8)(offset + 4);
        if (logging) {
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]PSN               : 0x%x\r\n", iso_hdr_second_word(hdr) & 0xffff);
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]ISO_SDU_LENGTH    : 0x%x\r\n", (iso_hdr_second_word(hdr) >> 16) & 0xfff);
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]PS                : 0x%x\r\n", iso_hdr_second_word(hdr) >> 30);
        }
    }
    sdu[2] = (u8)((iso_hdr_first_word(hdr) >> 16) + offset - 4);
    sdu[3] = (u8)((((iso_hdr_first_word(hdr) >> 16) & 0x3fff) + offset - 4) >> 8);
    if (logging)
        log_print(2, NULL, "[Info]: [LL_ISO]ISO data load length : 0x%x\r\n", ((iso_hdr_first_word(hdr) >> 16) & 0x3fff) + offset - 4);
    return offset;
}

u8 ll_iso_unpack_hdr(const u8 *sdu, hci_iso_hdr_t *hdr) LL_CODE;
u8 ll_iso_unpack_hdr(const u8 *sdu, hci_iso_hdr_t *hdr)
{
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]-----Unpack\r\n");
    hdr->handle = sdu[0] | ((sdu[1] << 8) & 0xf00);
    hdr->pb_flag = (sdu[1] >> 4) & 3;
    hdr->ts_flag = (sdu[1] >> 6) & 1;
    hdr->rfu = 0;
    hdr->iso_data_load_length = sdu[2] | ((sdu[3] << 8) & 0x3f00);
    const u8 *data = sdu + 4;
    u8 offset = 4;
    if (hdr->ts_flag) {
        u32 time_stamp = sdu[4] | (sdu[5] << 8) | (sdu[6] << 16) | ((u32)sdu[7] << 24);
        hdr->time_stamp = time_stamp;
        data = sdu + 8;
        offset = 8;
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]+[1]TS                : 0x%x\r\n", (int)time_stamp);
    }
    if (hdr->pb_flag == 0 || hdr->pb_flag == 2) {
        hdr->time_stamp = 0;
        u32 sequence = data[0] | (data[1] << 8);
        hdr->packet_sequence_num = sequence;
        hdr->iso_sdu_length = data[2] | ((data[3] << 8) & 0xf00);
        data += 4;
        offset = (u8)(offset + 4);
        if (log_tag_const_i_LL_ISO) {
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]PSN               : 0x%x\r\n", (int)sequence);
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]ISO_SDU_LENGTH    : 0x%x\r\n", (int)hdr->iso_sdu_length);
            log_print(2, NULL, "[Info]: [LL_ISO]+[2]PS                : 0x%x\r\n", (int)hdr->packet_status_flag);
        }
    }
    hdr->iso_sdu = (u8 *)data;
    return offset;
}

void ll_iso_cis_sdu_fragmentation(le_cis_t *cis, u8 *iso_data, u16 size) LL_CODE;
void ll_iso_cis_sdu_fragmentation(le_cis_t *cis, u8 *iso_data, u16 size)
{
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]frag_pos  0x%x /  sdu_size : 0x%x\r\n", iso_data, (int)size);
    /* The SDK's pdu pointer is never assigned and compiles to null: the PDU
     * header word is read once, rebuilt for each fragment and written back
     * once, and the payload goes to address 2. */
    u16 header = *(volatile u16 *)NULL_PDU;
    u8 *frag_pos = iso_data;
    u16 remaining = size;
    u16 next;
    do {
        u16 fragment = remaining > cis->pduSizeMToS ? cis->pduSizeMToS : remaining;
        next = (u16)(remaining - fragment);
        header = (u16)(((u16)(fragment << 8) | (header & 252)) | (next != 0));
        __builtin_memcpy(NULL_PDU + 2, frag_pos, fragment);
        frag_pos += fragment;
        remaining = next;
    } while (next != 0);
    *(volatile u16 *)NULL_PDU = header;
}

void hci_push_iso_sdu(u8 *packet, u16 size) LL_CODE;
void hci_push_iso_sdu(u8 *packet, u16 size)
{
    hci_iso_hdr_t sdu_hdr;
    (void)size;

    ll_iso_unpack_hdr(packet, &sdu_hdr);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]ISO SDU : %d\r\n");
        printf_buf(sdu_hdr.iso_sdu, sdu_hdr.iso_sdu_length);
    }
    u16 handle = (u16)(sdu_hdr.handle & 4095);
    le_cis_t *cis = ll_cis_by_handle(handle);
    if (!cis) {
        ll_bis_by_handle(handle);
        return;
    }
    if (ll_cis_is_established(cis) == 0) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]CIS not established\r\n");
        return;
    }
    if (cis->bnMToS == 0 && cis->role == 0) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]MToS no allow transmissions\r\n");
        return;
    }
    if (cis->bnSToM == 0 && cis->role == 1) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]SToM no allow transmissions\r\n");
        return;
    }
    if (cis->framing == 0)
        ll_iso_cis_sdu_fragmentation(cis, sdu_hdr.iso_sdu, (u16)(sdu_hdr.iso_sdu_length & 4095));
}

void lctrIsoSendCodecSdu(u16 handle) LL_CODE;
void lctrIsoSendCodecSdu(u16 handle)
{
    u32 pktCtr;
    le_bis_t *bis = ll_bis_by_handle(handle);
    if (!bis) {
        if (log_tag_const_d_LL_ISO)
            log_print(1, NULL, "[Debug]: [LL_ISO]lctrIsoSendCodecSdu: handle=%u not found\r\n", (int)handle);
        return;
    }
    u8 *buffer = malloc((u32)ll_iso_rt_cfg.maxIsoSduLen + 8);
    if (!buffer) {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]!!! Out of memory; dropping input SDU, stream=%u\r\n", (int)handle);
        return;
    }
    if (!ll_codec.in) btctrler_assert_failed("ll_codec.in", 0);
    if (ll_codec.in(handle, buffer + 12, bis->big->maxSdu, &pktCtr) == 0) {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]ISO audio stream data not available, stream=%u\r\n", (int)handle);
        free(buffer);
    }
}

static u8 ll_iso_pack_seg_hdr(u8 *pdu, const segm_hdr_t *seg_hdr) LL_CODE;
static u8 ll_iso_pack_seg_hdr(u8 *pdu, const segm_hdr_t *seg_hdr)
{
    u16 word;
    __builtin_memcpy(&word, seg_hdr, sizeof(word));
    pdu[0] = (u8)(word & 3);
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]Packe Seg sc : %d / cmplt : %d\r\n",
                  word & 1, (word >> 1) & 1);
    __builtin_memcpy(&word, seg_hdr, sizeof(word));
    if (word & 1) {
        pdu[1] = (u8)(word >> 8);
        return 2;
    }
    pdu[1] = (u8)((word >> 8) + 3);
    pdu[2] = (u8)seg_hdr->timeoffset;
    pdu[3] = (u8)(seg_hdr->timeoffset >> 8);
    pdu[4] = (u8)(seg_hdr->timeoffset >> 16);
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]Seg timeoffset : 0x%x\r\n", (int)seg_hdr->timeoffset);
    return 5;
}

void ll_iso_init(void) LL_CODE;
void ll_iso_init(void)
{
    hci_param.hci_version = 11;
    hci_param.lmp_pal_version = 11;
    *(iso_wire_u64 *)(void *)&hci_param = 2305983746735705236ULL;
    *(iso_wire_u64 *)(void *)&le_param = 17172529151ULL;
    ll_iso_hdl.maxIsoPduLen = 251;

    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]VS Codec init\r\n");
    ll_codec.start = PalCodecDataStartStream;
    ll_codec.stop = PalCodecDataStopStream;
    ll_codec.in = PalCodecDataStreamIn;
    ll_codec.out = PalCodecDataStreamOut;
    ll_bis_init();
    ll_cis_init();
    ll_iso_msg_queue_init();
    (void)thread_create(&ll_iso_hdl.thread, "isoal", ll_iso_process, &bt_task_thread);
}

void ll_iso_destroy(void) LL_CODE;
void ll_iso_destroy(void)
{
    ll_bis_close();
    ll_cis_close();
    ll_iso_msg_queue_destroy();
}

void ll_bis_process(void) LL_CODE;
void ll_bis_process(void)
{
    le_bis_t *bis = (le_bis_t *)ll_iso_hdl.bis_head.next;
    while (&bis->entry != &ll_iso_hdl.bis_head)
        bis = (le_bis_t *)bis->entry.next;
}

void ll_iso_process(struct thread *th) LL_CODE;
void ll_iso_process(struct thread *th)
{
    u32 start = sys_timer_get_ms();
    ll_msg_t *msg;

    while ((msg = ll_iso_msg_recv()) != NULL) {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]msg->state_id %d\r\n", msg->state_id);
        if (msg->state_id != 255) {
            ll_dispatch_msg_t handler = ll_dispatch_msg_table[msg->state_id];
            if (handler)
                handler((u8)msg->msg_id, msg->payload);
        }
        ll_iso_msg_free(msg);
    }

    u32 elapsed = sys_timer_get_ms() - start;
    if (elapsed > ll_iso_process_time_water_mark)
        ll_iso_process_time_water_mark = elapsed;
    thread_suspend(th, 0);
}

int le_cmd_set_iso(u16 opcode, const u8 *data, u32 size) LL_CODE;
int le_cmd_set_iso(u16 opcode, const u8 *data, u32 size)
{
    (void)size;
    switch (opcode & 0x3ff) {
    case 96:
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_READ_BUF_SIZE_V2\r\n");
        (void)hci_send_event_cmd_complete(opcode, "12121", 0,
                                          le_param.acl_data_packet_length,
                                          le_param.acl_packets_total_num,
                                          le_param.iso_data_packet_length,
                                          le_param.iso_packets_total_num);
        return 1;
    case 97: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_READ_ISO_TX_SYNC\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        /* The SDK passes an undefined sequence in its not-found reply. */
        u16 packet_sequence;
        u8 status;
        le_cis_t *cis = ll_cis_by_handle(handle);
        if (cis) {
            packet_sequence = cis->cisEvtCounter;
            status = 0;
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                packet_sequence = (u16)(bis->big->bn * bis->big->eventCounter);
                status = 0;
            } else {
                if (log_tag_const_d_LL_ISO)
                    log_print(1, NULL, "[Debug]: [LL_ISO]LctrReadIsoTxSync: handle=%u not found\r\n", handle);
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12243", status, handle,
                                          packet_sequence & 0xffff, -1, 0);
        return 1;
    }
    case 110: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_SETUP_ISO_DATA_PATH\r\n");
        u16 handle = data[0] | (data[1] << 8);
        u8 direction = data[2];
        u8 id = data[3];
        if (log_tag_const_i_LL_ISO) {
            /* The SDK decodes the diagnostic fields before any log callback. */
            u8 codec_format = data[4], config_length = data[12];
            u16 codec_company = data[5] | (data[6] << 8);
            u16 codec_id = data[7] | (data[8] << 8);
            u32 controller_delay = data[9] | (data[10] << 8) | (data[11] << 16);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> handle : 0x%x\r\n", handle);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> dpDir  : 0x%x\r\n", direction);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> dpId   : 0x%x\r\n", id);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> codecFormat: 0x%x\r\n", codec_format);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> codecCompId: 0x%x\r\n",
                      codec_company);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> codecId : 0x%x\r\n",
                      codec_id);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> ctrDly : 0x%x\r\n",
                      controller_delay);
            log_print(2, NULL, "[Info]: [LL_ISO]setup data path -> codecConfigLen : 0x%x\r\n", config_length);
            printf_buf(data + 13, config_length);
        }

        u8 status;
        le_cis_t *cis = ll_cis_by_handle(handle);
        if (cis) {
            if (direction == 0) {
                if (cis->dataPathInCtx.id != id)
                    cis->dataPathInCtx.id = (LlIsoDataPath_t)id;
                status = 0;
            } else if (direction == 1) {
                if (cis->dataPathOutCtx.id != id) {
                    cis->dataPathOutCtx.id = (LlIsoDataPath_t)id;
                    if (id == 0)
                        cis->dataPathOutCtx.cfg.hci.numRxPend = 0;
                }
                status = 0;
            } else {
                if (log_tag_const_d_LL_ISO)
                    log_print(1, NULL,
                              "[Debug]: [LL_ISO]setup data path handle=%u invalid direction dpDir=%u\r\n",
                              handle, direction);
                status = 12;
            }
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                if (direction == 0) {
                    if (bis->big->role == 1) {
                        status = ll_bis_setup_iso_data_path(bis, direction, id);
                    } else {
                        if (log_tag_const_d_LL_ISO)
                            log_print(1, NULL,
                                      "[Debug]: [LL_ISO]LctrSetupIsoDataPath: handle=%u invalid input direction for master\r\n",
                                      handle);
                        status = 12;
                    }
                } else if (direction == 1) {
                    if (bis->big->role == 0) {
                        status = ll_bis_setup_iso_data_path(bis, direction, id);
                    } else {
                        if (log_tag_const_d_LL_ISO)
                            log_print(1, NULL,
                                      "[Debug]: [LL_ISO]LctrSetupIsoDataPath: handle=%u invalid output direction for slave\r\n",
                                      handle);
                        status = 12;
                    }
                } else {
                    status = 0; /* SDK accepts an unrecognised BIS direction. */
                }
            } else {
                if (log_tag_const_d_LL_ISO)
                    log_print(1, NULL, "[Debug]: [LL_ISO]LctrSetupIsoDataPath: handle=%u not found\r\n", handle);
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12", status, handle);
        return 1;
    }
    case 111: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_REMOVE_ISO_DATA_PATH\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        u8 status;
        le_cis_t *cis = ll_cis_by_handle(handle);
        if (cis) {
            if (cis->dataPathInCtx.id == LL_ISO_DATA_PATH_DISABLED ||
                cis->dataPathOutCtx.id == LL_ISO_DATA_PATH_DISABLED) {
                status = 12;
            } else {
                cis->dataPathInCtx.id = LL_ISO_DATA_PATH_DISABLED;
                cis->dataPathOutCtx.id = LL_ISO_DATA_PATH_DISABLED;
                status = 0;
            }
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                bis->path = LL_ISO_DATA_PATH_DISABLED;
                status = 0;
            } else {
                if (log_tag_const_d_LL_ISO)
                    log_print(1, NULL, "[Debug]: [LL_ISO]LctrRemoveIsoDataPath: handle=%u not found\r\n", handle);
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12", status, handle);
        return 1;
    }
    case 112: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_ISO_TX_TEST\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        u8 payload_type = data[2];
        le_cis_t *cis = ll_cis_by_handle(handle);
        u8 status;
        if (cis) {
            status = ll_cis_tx_test(cis, payload_type);
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                status = ll_bis_tx_test(bis, payload_type);
            } else {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL, "<Error>: [LL_ISO]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12", status, handle);
        return 1;
    }
    case 113: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_ISO_RX_TEST\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        u8 payload_type = data[2];
        le_cis_t *cis = ll_cis_by_handle(handle);
        u8 status;
        if (cis) {
            status = ll_cis_rx_test(cis, payload_type);
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                status = ll_bis_rx_test(bis, payload_type);
            } else {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL, "<Error>: [LL_ISO]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12", status, handle);
        return 1;
    }
    case 114: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_ISO_READ_TEST_COUNTERS\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        /* Unknown handles still pass the SDK's uninitialised counters. */
        LlIsoTestCtrs_t counters;
        le_cis_t *cis = ll_cis_by_handle(handle);
        u8 status;
        if (cis) {
            (void)ll_cis_read_test_counters(cis, &counters);
            status = 0;
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                (void)ll_bis_read_test_counters(bis, &counters);
                status = 0;
            } else {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL, "<Error>: [LL_ISO]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12444", status, handle,
                                          counters.numSuccess, counters.numMissed, counters.numFailed);
        return 1;
    }
    case 115: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_ISO_TEST_END\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        /* Transmit-only test end leaves these counters undefined, as in IR. */
        LlIsoTestCtrs_t counters;
        le_cis_t *cis = ll_cis_by_handle(handle);
        u8 status = 0;
        if (cis) {
            if (cis->rxTestEnabled) {
                (void)ll_cis_read_test_counters(cis, &counters);
                cis->rxTestEnabled = 0;
            }
            if (cis->txTestEnabled)
                cis->txTestEnabled = 0;
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                (void)ll_bis_read_test_counters(bis, &counters);
                bis->test.term = 1;
            } else {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL, "<Error>: [LL_ISO]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "12444", status, handle,
                                          counters.numSuccess, counters.numMissed, counters.numFailed);
        return 1;
    }
    case 117: {
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]HCI_LE_READ_ISO_LINK_QUAL\r\n");
        u16 handle = btctler_little_endian_read_16((u8 *)data, 0);
        /* A BIS fills only crcErrPkt and rxUnreceivedPkt; the rest are undefined. */
        u32 stats[7];
        le_cis_t *cis = ll_cis_by_handle(handle);
        u8 status = 0;
        if (cis) {
            stats[0] = cis->isoLinkQualStats.txUnAckPkt;
            stats[1] = cis->isoLinkQualStats.txFlushedPkt;
            stats[2] = cis->isoLinkQualStats.txLastSubEventPkt;
            stats[3] = cis->isoLinkQualStats.retransmitPkt;
            stats[4] = cis->isoLinkQualStats.crcErrPkt;
            stats[5] = cis->isoLinkQualStats.rxUnreceivedPkt;
            stats[6] = cis->isoLinkQualStats.duplicatePkt;
        } else {
            le_bis_t *bis = ll_bis_by_handle(handle);
            if (bis) {
                stats[4] = bis->crcErrPkt;
                stats[5] = bis->rxUnreceivedPkt;
            } else {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL, "<Error>: [LL_ISO]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
                status = 2;
            }
        }
        (void)hci_send_event_cmd_complete(opcode, "124444444", status, handle,
                                          stats[0], stats[1], stats[2], stats[3],
                                          stats[4], stats[5], stats[6]);
        return 1;
    }
    default:
        return 0;
    }
}

void ll_iso_cis_sdu_recombination(le_cis_t *cis, cis_pdu_t *pdu) LL_CODE;
void ll_iso_cis_sdu_recombination(le_cis_t *cis, cis_pdu_t *pdu)
{
    hci_iso_hdr_t hdr;
    lctrIsoalRxCtx_t *rx = &cis->isoalRxCtx;
    u16 boundary = 0;
    u16 llid = pdu->llid;

    if (llid == 0) {
        if (rx->rxState == 0) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]4-LCTR_PB_COMP\r\n");
        } else if (rx->rxState == 1) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]3-LCTR_PB_LAST\r\n");
        }
        boundary = rx->rxState == 0 ? 0x2000 : 0x3000;
        rx->rxState = 0;
    } else if (llid == 1) {
        if (rx->rxState == 0) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]2-LCTR_PB_FIRST\r\n");
        } else if (rx->rxState == 1) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]1-LCTR_PB_CONT\r\n");
        }
        boundary = rx->rxState == 0 ? 0 : 0x1000;
        rx->rxState = 1;
    }

    rx->data.unframed.curLen = (u16)(rx->data.unframed.curLen + pdu->length);
    if (rx->pduFlushed) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]Flush Timeout\r\n");
        if (rx->rxState == 0) {
            rx->data.unframed.ps = llid == 0 ? 1 : 2;
        } else {
            rx->data.unframed.ps = 1;
        }
        rx->pduFlushed = 0;
    }

    hdr.handle = cis->cisHandle;
    hdr.pb_flag = (boundary >> 12) & 3;
    hdr.ts_flag = (boundary == 0 || boundary == 0x2000);
    hdr.rfu = 0;
    hdr.iso_data_load_length = pdu->length;
    hdr.rfu2 = 0;
    /* The SDK leaves the timestamp and SDU subheader uninitialised. */

    u8 *packet = (u8 *)pdu - 6 * sizeof(*pdu);
    u8 offset = ll_iso_pack_hdr(packet, &hdr);
    __builtin_memmove(packet + offset, pdu->payload, pdu->length);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]-SDU offset 0x%x\r\n", offset);
        printf_buf(packet, pdu->length + offset);
    }
    lbuf_push_btctrler(packet);
    if (rx->rxState != 0)
        return;

    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]>>>>>>>>>Emit SDU to Host\r\n");
    u8 *peek = lbuf_peek_btctrler((struct lbuff_head_btctrler *)rx->data.unframed.pendSduQ);
    (void)ll_iso_unpack_hdr(peek, &hdr);
    u32 first = iso_hdr_first_word(&hdr);
    u32 adjusted = ((first + 0x3ff80000u) & 0x3fff0000u) |
                   (first & 0xc000ffffu);
    iso_hdr_set_first_word(&hdr, adjusted);
    if (first & 0x4000)
        hdr.time_stamp = 0xff;

    u16 sequence = rx->packetSequence++;
    u32 second = iso_hdr_second_word(&hdr);
    second = ((u32)rx->data.unframed.ps << 30) |
             ((second & 0x3fff0000u) | sequence);
    u16 sdu_length = (u16)(rx->data.unframed.curLen & 0xfff);
    second = ((u32)sdu_length << 16) | (second & 0xf000ffffu);
    iso_hdr_set_second_word(&hdr, second);
    if (sdu_length > cis->sduSizeMToS) {
        if (log_tag_const_e_LL_ISO)
            log_print(4, NULL, "<Error>: [LL_ISO]SDU exceeding Max_SDU 0x%x \r\n", cis->sduSizeMToS);
        second = ((u32)(cis->sduSizeMToS & 0xfff) << 16) | (second & 0xf000ffffu);
        iso_hdr_set_second_word(&hdr, second);
    }
    if (rx->data.unframed.ps == 1) {
        second &= 0xf000ffffu;
        iso_hdr_set_second_word(&hdr, second);
    }

    (void)ll_iso_pack_hdr(peek, &hdr);
    u8 *packet_to_host;
    while ((packet_to_host = lbuf_pop_btctrler(
                (struct lbuff_head_btctrler *)rx->data.unframed.pendSduQ)) != NULL) {
        if (cis->dataPathOutCtx.id == LL_ISO_DATA_PATH_HCI) {
            u8 offset_to_host = ll_iso_unpack_hdr(packet_to_host, &hdr);
            u32 pack_len = ((iso_hdr_first_word(&hdr) >> 16) & 0x3fff) + 4;
            if (log_tag_const_i_LL_ISO) {
                log_print(2, NULL, "[Info]: [LL_ISO]<- LL_ISO_DATA_PATH_HCI offset %d\r\n", offset_to_host);
                printf_buf(packet_to_host, pack_len);
            }
            (void)h4_transport->send_packet(2, packet_to_host, pack_len);
        }
        lbuf_free_btctrler(packet_to_host);
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]Free\r\n");
    }
}

void ll_iso_cis_sdu_segmentation(le_cis_t *cis, u8 *iso_data, u16 sdu_size) LL_CODE;
void ll_iso_cis_sdu_segmentation(le_cis_t *cis, u8 *iso_data, u16 sdu_size)
{
    (void)cis;
    (void)iso_data;
    (void)sdu_size;
}

void ll_iso_cis_sdu_reassembly(le_cis_t *cis, cis_pdu_t *pdu) LL_CODE;
void ll_iso_cis_sdu_reassembly(le_cis_t *cis, cis_pdu_t *pdu)
{
    hci_iso_hdr_t hdr;
    lctrIsoalRxCtx_t *rx = &cis->isoalRxCtx;

    if (rx->pduFlushed && rx->pPendSduBuf) {
        free(rx->pPendSduBuf);
        rx->pPendSduBuf = NULL;
    }

    u16 pdu_size = pdu->length;
    u8 remaining = (u8)pdu_size;
    u8 *position = pdu->payload;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]PDU size %d\r\n", pdu_size);

    u16 segment_word = 0;
    while (remaining != 0) {
        u8 first = position[0];
        u8 starts_segment = (first & 1) == 0;
        u8 complete = (first & 2) != 0;
        u8 length = position[1];
        segment_word = (u16)((segment_word & 0xfc) | (first & 3) | (length << 8));
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]Unpacke Seg sc : %d / cmplt : %d\r\n",
                      first & 1, complete);
        u8 header_size = starts_segment ? 5 : 2;
        if (log_tag_const_i_LL_ISO && starts_segment)
            log_print(2, NULL, "[Info]: [LL_ISO]Seg timeoffset : 0x%x\r\n",
                      position[2] | (position[3] << 8) | (position[4] << 16));

        if (rx->rxState == 0) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]Seg Alloc size 0x%x\r\n",
                          ll_iso_rt_cfg.maxIsoSduLen + 12);
            rx->pPendSduBuf = malloc((u32)ll_iso_rt_cfg.maxIsoSduLen + 12);
            isoal_alloc_assert(rx->pPendSduBuf, (u32)ll_iso_rt_cfg.maxIsoSduLen + 12);
            __builtin_memset(rx->pPendSduBuf, 0, (u32)ll_iso_rt_cfg.maxIsoSduLen + 12);
            rx->rxSduOffset = 12;
            if (!starts_segment) {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL,
                              "<Error>: [LL_ISO]Missed start of segmetation, dropping 0x%x\r\n",
                              cis->cisHandle);
                free(rx->pPendSduBuf);
                rx->pPendSduBuf = NULL;
                u32 skip = header_size + length;
                position += skip;
                remaining = (u8)(remaining - skip);
                continue;
            }
            rx->data.framed.timeOffset = btctler_little_endian_read_24(pdu->payload, 2);
            segment_word = (u16)(segment_word - 0x300);
        } else if (rx->rxState == 1) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]LL_ISO_SDU_STATE_CONT\r\n");
            if (starts_segment) {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL,
                              "<Error>: [LL_ISO]Expected continue segmetation, dropping 0x%x\r\n",
                              cis->cisHandle);
                free(rx->pPendSduBuf);
                rx->pPendSduBuf = NULL;
                u32 skip = header_size + length;
                position += skip;
                remaining = (u8)(remaining - skip);
                continue;
            }
        } else {
            if (log_tag_const_e_LL_ISO)
                log_print(4, NULL, "<Error>: [LL_ISO]Error State\r\n");
            free(rx->pPendSduBuf);
            rx->pPendSduBuf = NULL;
            u32 skip = header_size + length;
            position += skip;
            remaining = (u8)(remaining - skip);
            continue;
        }

        u16 segment_length = segment_word >> 8;
        __builtin_memcpy(rx->pPendSduBuf + rx->rxSduOffset, position + header_size, segment_length);
        if (log_tag_const_i_LL_ISO) {
            log_print(2, NULL, "[Info]: [LL_ISO]Sdu offset 0x%x\r\n", rx->rxSduOffset);
            printf_buf(rx->pPendSduBuf, rx->rxSduOffset + segment_length);
        }
        rx->rxSduOffset = (u16)(rx->rxSduOffset + segment_length);
        u32 consumed = header_size + segment_length;
        position += consumed;
        remaining = (u8)(remaining - consumed);
        if (log_tag_const_i_LL_ISO)
            log_print(2, NULL, "[Info]: [LL_ISO]offset %d, length %d\r\n",
                      header_size, segment_length);

        if (complete) {
            if (log_tag_const_i_LL_ISO)
                log_print(2, NULL, "[Info]: [LL_ISO]Emit to Host\r\n");
            u32 payload_length = (rx->rxSduOffset - 12) & 0xfff;
            hdr.handle = cis->cisHandle;
            hdr.pb_flag = 2;
            hdr.ts_flag = 1;
            hdr.rfu = 0;
            hdr.iso_data_load_length = (u16)payload_length;
            hdr.rfu2 = 0;
            hdr.time_stamp = 0xff;
            u16 sequence = rx->packetSequence++;
            u32 second = iso_hdr_second_word(&hdr);
            second = (second & 0xf0000000u) | sequence |
                     ((payload_length & 0xfff) << 16);
            iso_hdr_set_second_word(&hdr, second);
            if (payload_length > cis->sduSizeMToS) {
                if (log_tag_const_e_LL_ISO)
                    log_print(4, NULL, "<Error>: [LL_ISO]SDU exceeding Max_SDU\r\n");
                second = ((cis->sduSizeMToS & 0xfff) << 16) |
                         (second & 0xf000ffffu);
                iso_hdr_set_second_word(&hdr, second);
            }
            u8 output_offset = ll_iso_pack_hdr(rx->pPendSduBuf, &hdr);
            if (cis->dataPathOutCtx.id == LL_ISO_DATA_PATH_HCI) {
                u32 output_length = payload_length + output_offset;
                if (log_tag_const_i_LL_ISO) {
                    log_print(2, NULL,
                              "[Info]: [LL_ISO]<- LL_ISO_DATA_PATH_HCI SDU 0x%x\r\n",
                              output_offset);
                    printf_buf(rx->pPendSduBuf, output_length);
                }
                (void)h4_transport->send_packet(2, rx->pPendSduBuf, output_length);
            }
            rx->rxState = 0;
        } else {
            rx->rxState = 1;
        }
    }
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]--end\r\n");
}

static void isoal_test_add_tail(struct list_head *entry)
{
    struct list_head *head = &ll_iso_hdl.cis_head;
    struct list_head *previous = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = previous;
    previous->next = entry;
}

static __inline__ __attribute__((always_inline)) void isoal_cis_fragmentation_test(void)
{
    u8 sdu[256];
    le_cis_t cis;
    hci_iso_hdr_t hdr;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]>>>>>>>>>>ISOAL Fragmentation\r\n");
    __builtin_memset(sdu, 0, sizeof(sdu));
    __builtin_memset(&cis, 0, sizeof(cis));
    isoal_test_add_tail(&cis.entry);
    cis.cisHandle = 1;
    cis.framing = 0;
    cis.state = 1;
    cis.bnMToS = 1;
    cis.role = 0;
    cis.pduSizeMToS = 4;
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    cis.isoalRxCtx.rxState = 0;
    cis.isoalRxCtx.data.unframed.ps = 0;
    iso_hdr_set_first_word(&hdr, 0xb6001);
    hdr.time_stamp = 0x1234;
    iso_hdr_set_second_word(&hdr, 0xb0001);
    u8 offset = ll_iso_pack_hdr(sdu, &hdr);
    __builtin_memcpy(sdu + offset, "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a", 11);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]pSduBuf 0x%x , sdu_hdr.iso_sdu 0x%x\r\n",
                  sdu, hdr.iso_sdu);
        log_print(2, NULL, "[Info]: [LL_ISO]HCI ISO : \r\n");
        printf_buf(sdu, offset + 11);
    }
    hci_push_iso_sdu(sdu, 0);
}

static __inline__ __attribute__((always_inline)) void isoal_cis_recombination_test(void)
{
    u8 queue_memory[256];
    le_cis_t cis;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]>>>>>>>>>>ISOAL Recombination\r\n");
    __builtin_memset(queue_memory, 0, sizeof(queue_memory));
    __builtin_memset(&cis, 0, sizeof(cis));
    cis.cisHandle = 1;
    cis.sduSizeMToS = 64;
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    cis.isoalRxCtx.rxState = 0;
    cis.isoalRxCtx.data.unframed.pendSduQ =
        (struct lbuff_head *)lbuf_init_btctrler(queue_memory, 256);
    cis.isoalRxCtx.data.unframed.ps = 0;

    u8 *sample1 = lbuf_alloc_btctrler(
        (struct lbuff_head_btctrler *)cis.isoalRxCtx.data.unframed.pendSduQ, 18);
    cis_pdu_t *pdu1 = (cis_pdu_t *)(sample1 + 12);
    pdu1->llid = 1;
    pdu1->length = 4;
    *(iso_wire_u32 *)pdu1->payload = 0x04030201;
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU0 0x%x\r\n", sample1);
        printf_buf(sample1, 18);
    }
    ll_iso_cis_sdu_recombination(&cis, pdu1);

    u8 *sample2 = lbuf_alloc_btctrler(
        (struct lbuff_head_btctrler *)cis.isoalRxCtx.data.unframed.pendSduQ, 19);
    cis_pdu_t *pdu2 = (cis_pdu_t *)(sample2 + 12);
    pdu2->llid = 1;
    pdu2->length = 5;
    __builtin_memcpy(pdu2->payload, "\x05\x06\x07\x08\x09", 5);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU1 0x%x\r\n", sample2);
        printf_buf(sample2, 19);
    }
    ll_iso_cis_sdu_recombination(&cis, pdu2);

    u8 *sample3 = lbuf_alloc_btctrler(
        (struct lbuff_head_btctrler *)cis.isoalRxCtx.data.unframed.pendSduQ, 20);
    cis_pdu_t *pdu3 = (cis_pdu_t *)(sample3 + 12);
    pdu3->llid = 0;
    pdu3->length = 6;
    __builtin_memcpy(pdu3->payload, "\x0a\x0b\x0c\x0d\x0e\x0f", 6);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU2 0x%x\r\n", sample3);
        printf_buf(sample3, 20);
    }
    ll_iso_cis_sdu_recombination(&cis, pdu3);
}

static __inline__ __attribute__((always_inline)) void isoal_cis_segmentation_test(void)
{
    u8 sdu[256];
    le_cis_t cis;
    hci_iso_hdr_t hdr;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]>>>>>>>>>>ISOAL Segmentation\r\n");
    __builtin_memset(sdu, 0, sizeof(sdu));
    __builtin_memset(&cis, 0, sizeof(cis));
    isoal_test_add_tail(&cis.entry);
    cis.cisHandle = 1;
    cis.framing = 1;
    cis.state = 1;
    cis.role = 0;
    cis.pduSizeMToS = 32;
    cis.sduSizeMToS = 128;
    cis.isoInterval = 8;
    cis.sduIntervalMToS = 15000;
    cis.bnMToS = 4;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]BN(size) %d \r\n", cis.bnMToS);
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    cis.isoalRxCtx.rxState = 0;
    iso_hdr_set_first_word(&hdr, 0xb6001);
    hdr.time_stamp = 0x1234;
    iso_hdr_set_second_word(&hdr, 0xb0001);
    u8 offset = ll_iso_pack_hdr(sdu, &hdr);
    __builtin_memcpy(sdu + offset, "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a", 11);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]pSduBuf 0x%x , sdu_hdr.iso_sdu 0x%x\r\n",
                  sdu, hdr.iso_sdu);
        log_print(2, NULL, "[Info]: [LL_ISO]HCI ISO : \r\n");
        printf_buf(sdu, offset + 11);
    }
    hci_push_iso_sdu(sdu, 0);
}

static __inline__ __attribute__((always_inline)) void isoal_cis_reassembly_test(void)
{
    u8 queue_memory[256];
    le_cis_t cis;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]>>>>>>>>>>ISOAL Reassembly\r\n");
    __builtin_memset(queue_memory, 0, sizeof(queue_memory));
    __builtin_memset(&cis, 0, sizeof(cis));
    cis.cisHandle = 1;
    cis.role = 0;
    cis.pduSizeMToS = 32;
    cis.sduSizeMToS = 128;
    cis.isoInterval = 8;
    cis.sduIntervalMToS = 15000;
    cis.bnMToS = 1;
    if (log_tag_const_i_LL_ISO)
        log_print(2, NULL, "[Info]: [LL_ISO]BN %d \r\n", cis.bnMToS);
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    cis.isoalRxCtx.rxState = 0;
    struct lbuff_head_btctrler *queue = lbuf_init_btctrler(queue_memory, 256);

    u8 *sample1 = lbuf_alloc_btctrler(queue, 27);
    cis_pdu_t *pdu1 = (cis_pdu_t *)(sample1 + 12);
    pdu1->length = 13;
    pdu1->llid = 2;
    segm_hdr_t header1 = {0};
    header1.length = 8;
    header1.timeoffset = 0x112233;
    u8 header_size = ll_iso_pack_seg_hdr((u8 *)(sample1 + 14), &header1);
    *(iso_wire_u64 *)(void *)(sample1 + 14 + header_size) = 0x0807060504030201ULL;
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU1 0x%x\r\n", sample1);
        printf_buf(sample1, 27);
    }
    ll_iso_cis_sdu_reassembly(&cis, pdu1);
    lbuf_free_btctrler(sample1);

    u8 *sample2 = lbuf_alloc_btctrler(queue, 28);
    cis_pdu_t *pdu2 = (cis_pdu_t *)(sample2 + 12);
    pdu2->length = 14;
    pdu2->llid = 2;
    u8 *segment = sample2 + 14;
    segm_hdr_t header2 = {0};
    header2.sc = 1;
    header2.cmplt = 1;
    header2.length = 3;
    header_size = ll_iso_pack_seg_hdr(segment, &header2);
    __builtin_memcpy(segment + header_size, "\x09\x0a\x0b", 3);
    segment += header_size + 3;
    header2.sc = 0;
    header2.cmplt = 0;
    header2.length = 4;
    header2.timeoffset = 0x445566;
    header_size = ll_iso_pack_seg_hdr(segment, &header2);
    *(iso_wire_u32 *)(void *)(segment + header_size) = 0x13121110;
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU2 0x%x\r\n", sample2);
        printf_buf(sample2, 28);
    }
    ll_iso_cis_sdu_reassembly(&cis, pdu2);
    lbuf_free_btctrler(sample2);

    u8 *sample3 = lbuf_alloc_btctrler(queue, 26);
    cis_pdu_t *pdu3 = (cis_pdu_t *)(sample3 + 12);
    pdu3->length = 10;
    pdu3->llid = 2;
    segm_hdr_t header3 = {0};
    header3.sc = 1;
    header3.length = 10;
    header_size = ll_iso_pack_seg_hdr((u8 *)(sample3 + 14), &header3);
    __builtin_memcpy(sample3 + 14 + header_size, "\x14\x15\x16\x17\x18\x19\x20\x21\x22\x23", 10);
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU3 0x%x\r\n", sample3);
        printf_buf(sample3, 26);
    }
    ll_iso_cis_sdu_reassembly(&cis, pdu3);
    lbuf_free_btctrler(sample3);

    u8 *sample4 = lbuf_alloc_btctrler(queue, 28);
    cis_pdu_t *pdu4 = (cis_pdu_t *)(sample4 + 12);
    pdu4->length = 14;
    pdu4->llid = 2;
    segment = sample4 + 14;
    header2.sc = 1;
    header2.cmplt = 1;
    header2.length = 3;
    header_size = ll_iso_pack_seg_hdr(segment, &header2);
    __builtin_memcpy(segment + header_size, "\x24\x25\x26", 3);
    segment += header_size + 3;
    header2.sc = 0;
    header2.cmplt = 1;
    header2.length = 4;
    header2.timeoffset = 0xaabbcc;
    header_size = ll_iso_pack_seg_hdr(segment, &header2);
    *(iso_wire_u32 *)(void *)(segment + header_size) = 0x33323130;
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]--PDU4 0x%x\r\n", sample4);
        printf_buf(sample4, 28);
    }
    ll_iso_cis_sdu_reassembly(&cis, pdu4);
    lbuf_free_btctrler(sample4);
}

void ll_isoal_test(void) __attribute__((noreturn)) LL_CODE;
void ll_isoal_test(void)
{
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]-------------------------------\r\n");
        log_print(2, NULL, "[Info]: [LL_ISO]----------ISOAL Test ----------\r\n");
        log_print(2, NULL, "[Info]: [LL_ISO]-------------------------------\r\n");
    }
    isoal_cis_fragmentation_test();
    isoal_cis_recombination_test();
    isoal_cis_segmentation_test();
    isoal_cis_reassembly_test();
    if (log_tag_const_i_LL_ISO) {
        log_print(2, NULL, "[Info]: [LL_ISO]-------------------------------\r\n");
        log_print(2, NULL, "[Info]: [LL_ISO]----------ISOAL Test END-------\r\n");
        log_print(2, NULL, "[Info]: [LL_ISO]-------------------------------\r\n");
    }
    while (1)
        ;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#undef malloc
#undef free

u8 log_tag_const_i_LL_ISO, log_tag_const_e_LL_ISO, log_tag_const_d_LL_ISO;
static u8 null_pdu_area[64];
u8 *ll_iso_null_pdu = null_pdu_area;

static int logs_i, logs_e, logs_d, printf_buf_len, malloc_size, malloc_fail, free_count;
static segm_hdr_t *mutate_segment;
static hci_iso_hdr_t *mutate_header;
static void *freed_ptr, *malloc_ptr;
static char trace[256];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }
static le_cis_t cis;
static le_bis_t bis;
static le_big_t big;
static int cis_established = 1;
static int bis_calls, bis_handle;
static u16 cis_handle_seen;
struct hci_parameter hci_param;
struct le_parameter le_param;
struct thread_owner bt_task_thread;
int config_asser;

static hci_transport_t test_transport;
const hci_transport_t *h4_transport = &test_transport;
static u8 sent_packet[128];
static u8 sent_packets[4][128];
static int sent_lengths[4];
static int sent_type, sent_len, send_calls;
static int bis_init_calls, cis_init_calls, bis_close_calls, cis_close_calls;
static int iso_queue_init_calls, iso_queue_destroy_calls;
static int thread_create_calls, thread_suspend_calls;
static struct thread *created_thread;
static void (*created_entry)(struct thread *);
static char created_name[16];
static u32 timer_values[8];
static int timer_count, timer_index;
static struct {
    u16 state_id;
    u16 msg_id;
    u8 payload[8];
} iso_messages[4];
static int iso_message_count, iso_message_head, iso_message_free_calls;
static int dispatch_calls;
static u16 dispatch_state, dispatch_payload;
static int event_calls, event_opcode, event_arg_count;
static char event_format[16];
static int event_args[12];
static u8 cis_tx_result = 7, cis_rx_result = 8, bis_tx_result = 9, bis_rx_result = 10;
static int cis_test_calls, bis_test_calls, counter_calls, setup_path_calls;
static u8 setup_path_result = 19;
static u16 setup_path_direction, setup_path_id;
static LlIsoTestCtrs_t counter_result;

static void *lbuf_queue[16];
static int lbuf_queue_count, lbuf_alloc_calls, lbuf_push_calls, lbuf_pop_calls, lbuf_free_calls;

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)tag;
    if (mutate_header && strstr(format, "-----Pack")) {
        mutate_header->ts_flag = 1; mutate_header->pb_flag = 2;
        mutate_header->time_stamp = 0xaabbccdd;
        mutate_header->packet_sequence_num = 0x1122; mutate_header->iso_sdu_length = 7;
        mutate_header = NULL;
    }
    if (mutate_segment && strstr(format, "Packe Seg sc")) {
        mutate_segment->sc = 0; mutate_segment->length = 7;
        mutate_segment->timeoffset = 0x332211; mutate_segment = NULL;
    }
    if (level == 4) logs_e++; else if (level == 1) logs_d++; else logs_i++;
}
void printf_buf(const void *b, int n) { (void)b; printf_buf_len = n; T("pbuf"); }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; T("assert"); }
void *ll_iso_test_malloc(u32 size) { malloc_size = (int)size; return malloc_fail ? NULL : (malloc_ptr = calloc(1, size)); }
void ll_iso_test_free(void *p) { freed_ptr = p; free_count++; }
le_cis_t *ll_cis_by_handle(u16 h) { cis_handle_seen = h; return h == 5 ? &cis : NULL; }
u8 ll_cis_is_established(le_cis_t *c) { CHECK(c == &cis); return (u8)cis_established; }
le_bis_t *ll_bis_by_handle(u16 h) { bis_calls++; bis_handle = h; return h == 7 ? &bis : NULL; }
u8 ll_cis_tx_test(le_cis_t *c, u8 payload_type)
{
    CHECK(c == &cis && payload_type == 3);
    cis_test_calls++;
    return cis_tx_result;
}
u8 ll_cis_rx_test(le_cis_t *c, u8 payload_type)
{
    CHECK(c == &cis && payload_type == 3);
    cis_test_calls++;
    return cis_rx_result;
}
u8 ll_cis_read_test_counters(le_cis_t *c, LlIsoTestCtrs_t *counters)
{
    CHECK(c == &cis && counters);
    counter_calls++;
    *counters = counter_result;
    return 0x37;
}
u8 ll_bis_tx_test(le_bis_t *b, u8 payload_type)
{
    CHECK(b == &bis && payload_type == 3);
    bis_test_calls++;
    return bis_tx_result;
}
u8 ll_bis_rx_test(le_bis_t *b, u8 payload_type)
{
    CHECK(b == &bis && payload_type == 3);
    bis_test_calls++;
    return bis_rx_result;
}
u8 ll_bis_read_test_counters(le_bis_t *b, LlIsoTestCtrs_t *counters)
{
    CHECK(b == &bis && counters);
    counter_calls++;
    *counters = counter_result;
    return 0x37;
}
u8 ll_bis_setup_iso_data_path(le_bis_t *b, LlIsoDataPathDir_t direction, LlIsoDataPath_t id)
{
    CHECK(b == &bis);
    setup_path_calls++;
    setup_path_direction = (u16)direction;
    setup_path_id = (u16)id;
    return setup_path_result;
}
void ll_bis_init(void) { bis_init_calls++; }
void ll_bis_close(void) { bis_close_calls++; }
void ll_cis_init(void) { cis_init_calls++; }
void ll_cis_close(void) { cis_close_calls++; }
void ll_iso_msg_queue_init(void) { iso_queue_init_calls++; }
void ll_iso_msg_queue_destroy(void)
{
    iso_queue_destroy_calls++;
    iso_message_count = iso_message_head = 0;
}
void ll_iso_msg_free(void *message)
{
    CHECK(message);
    iso_message_free_calls++;
}
ll_msg_t *ll_iso_msg_recv(void)
{
    if (iso_message_head == iso_message_count)
        return NULL;
    return (ll_msg_t *)&iso_messages[iso_message_head++];
}
u32 sys_timer_get_ms(void)
{
    if (timer_index < timer_count)
        return timer_values[timer_index++];
    return timer_values[timer_count ? timer_count - 1 : 0];
}
int thread_create(struct thread *thread, char *name, void (*entry)(struct thread *),
                  struct thread_owner *owner)
{
    CHECK(owner == &bt_task_thread);
    thread_create_calls++;
    created_thread = thread;
    created_entry = entry;
    strncpy(created_name, name, sizeof(created_name) - 1);
    return 0;
}
void thread_suspend(struct thread *thread, int timeout)
{
    CHECK(thread == created_thread && timeout == 0);
    thread_suspend_calls++;
}
u8 PalCodecDataStartStream(u16 id, PalCodecSreamParam_t *param)
{
    (void)id;
    (void)param;
    return 0;
}
void PalCodecDataStopStream(u16 id) { (void)id; }
u16 PalCodecDataStreamIn(u16 id, u8 *buffer, u16 length, u32 *packet_counter)
{
    (void)id;
    (void)buffer;
    (void)length;
    (void)packet_counter;
    return 0;
}
void PalCodecDataStreamOut(u16 id, const u8 *buffer, u16 length, u32 packet_counter)
{
    (void)id;
    (void)buffer;
    (void)length;
    (void)packet_counter;
}
void p33_soft_reset(void) { T("reset"); }
void cpu_assert_debug(void) { T("debug"); }

int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list args;
    int argc = 0;
    if (!strcmp(format, "12"))
        argc = 2;
    else if (!strcmp(format, "12121") || !strcmp(format, "12243") || !strcmp(format, "12444"))
        argc = 5;
    else if (!strcmp(format, "124444444"))
        argc = 9;
    CHECK(argc != 0);
    event_calls++;
    event_opcode = opcode;
    strncpy(event_format, format, sizeof(event_format) - 1);
    event_arg_count = argc;
    va_start(args, format);
    for (int i = 0; i < argc; i++)
        event_args[i] = va_arg(args, int);
    va_end(args);
    return 0;
}

u16 btctler_little_endian_read_16(const u8 *data, int offset)
{
    return (u16)(data[offset] | (data[offset + 1] << 8));
}

u32 btctler_little_endian_read_24(const u8 *data, int offset)
{
    return data[offset] | (data[offset + 1] << 8) | ((u32)data[offset + 2] << 16);
}

struct lbuff_head_btctrler *lbuf_init_btctrler(void *buffer, u32 length)
{
    CHECK(buffer && (length == 64 || length == 256));
    lbuf_queue_count = 0;
    return (struct lbuff_head_btctrler *)buffer;
}

void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 length)
{
    CHECK(head);
    lbuf_alloc_calls++;
    return calloc(1, length + 16);
}

void lbuf_push_btctrler(void *packet)
{
    CHECK(lbuf_queue_count < (int)(sizeof(lbuf_queue) / sizeof(lbuf_queue[0])));
    lbuf_queue[lbuf_queue_count++] = packet;
    lbuf_push_calls++;
}

void *lbuf_peek_btctrler(struct lbuff_head_btctrler *head)
{
    CHECK(head && lbuf_queue_count > 0);
    return lbuf_queue[0];
}

void *lbuf_pop_btctrler(struct lbuff_head_btctrler *head)
{
    CHECK(head);
    if (lbuf_queue_count == 0)
        return NULL;
    void *packet = lbuf_queue[0];
    lbuf_queue_count--;
    memmove(lbuf_queue, lbuf_queue + 1, (size_t)lbuf_queue_count * sizeof(lbuf_queue[0]));
    lbuf_pop_calls++;
    return packet;
}

void lbuf_free_btctrler(void *packet)
{
    CHECK(packet);
    lbuf_free_calls++;
}

static int send_packet(int type, const u8 *packet, int length)
{
    CHECK(length <= (int)sizeof(sent_packet));
    memcpy(sent_packet, packet, (size_t)length);
    if (send_calls < 4) {
        memcpy(sent_packets[send_calls], packet, (size_t)length);
        sent_lengths[send_calls] = length;
    }
    sent_type = type;
    sent_len = length;
    send_calls++;
    return 0;
}

static void dispatch_message(u8 state, void *payload)
{
    dispatch_calls++;
    dispatch_state = state;
    dispatch_payload = *(u16 *)payload;
}

static u16 codec_result, codec_arg_handle, codec_arg_len;
static u8 *codec_arg_buf;
static u32 *codec_arg_ctr;
static u16 codec_in(u16 h, u8 *buf, u16 len, u32 *ctr) { codec_arg_handle = h; codec_arg_buf = buf; codec_arg_len = len; codec_arg_ctr = ctr; return codec_result; }

int main(void)
{
    hci_iso_hdr_t hdr;

    /* Unpack: complete SDU (pb 2), no timestamp.  Handle 0x123 (top bits of byte 1 ignored for the handle),
     * load length 12 (byte 3 high bits ignored), psn 0x0a0b, sdu length 8. */
    u8 a[16] = { 0x23, 0x21 | 0x80, 12, 0x00 | 0xc0, 0x0b, 0x0a, 0x08, 0xf0, 1, 2, 3, 4, 5, 6, 7, 8 };
    memset(&hdr, 0xff, sizeof(hdr));
    CHECK(ll_iso_unpack_hdr(a, &hdr) == 8);
    CHECK(hdr.handle == 0x123 && hdr.pb_flag == 2 && hdr.ts_flag == 0 && hdr.iso_data_load_length == 12);
    CHECK(hdr.time_stamp == 0 && hdr.packet_sequence_num == 0x0a0b && hdr.iso_sdu_length == 8 && hdr.iso_sdu == a + 8);
    CHECK(hdr.rfu == 0 && hdr.rfu2 == 3 && hdr.rfu3 == 3 && hdr.packet_status_flag == 3);   /* unwritten bits keep their old value */

    /* Timestamp and pb 0: the timestamp is read, then cleared by the SDU header parse. */
    u8 b[20] = { 0xbc, 0x0a | 0x40, 20, 0, 0x11, 0x22, 0x33, 0x44, 0x34, 0x12, 0x05, 0x0f };
    memset(&hdr, 0, sizeof(hdr));
    CHECK(ll_iso_unpack_hdr(b, &hdr) == 12);
    CHECK(hdr.handle == 0xabc && hdr.pb_flag == 0 && hdr.ts_flag == 1 && hdr.iso_data_load_length == 20);
    CHECK(hdr.time_stamp == 0 && hdr.packet_sequence_num == 0x1234 && hdr.iso_sdu_length == 0xf05 && hdr.iso_sdu == b + 12);

    /* Timestamp and pb 3 (last fragment): timestamp kept, no SDU header, SDU length untouched. */
    u8 c[12] = { 0x05, 0x30 | 0x40, 4, 0, 0x78, 0x56, 0x34, 0x12, 9, 9, 9, 9 };
    memset(&hdr, 0, sizeof(hdr));
    hdr.iso_sdu_length = 0x777;
    CHECK(ll_iso_unpack_hdr(c, &hdr) == 8);
    CHECK(hdr.pb_flag == 3 && hdr.ts_flag == 1 && hdr.time_stamp == 0x12345678 && hdr.iso_sdu_length == 0x777 && hdr.iso_sdu == c + 8);

    /* pb 1, no timestamp: just the four header bytes. */
    u8 d[8] = { 0x01, 0x10, 4, 0, 1, 2, 3, 4 };
    memset(&hdr, 0, sizeof(hdr));
    hdr.time_stamp = 0x55;
    CHECK(ll_iso_unpack_hdr(d, &hdr) == 4);
    CHECK(hdr.pb_flag == 1 && hdr.time_stamp == 0x55 && hdr.iso_sdu == d + 4);

    /* Logging only when the info tag is set (PS reads the unwritten bits). */
    log_tag_const_i_LL_ISO = 1;
    logs_i = 0;
    ll_iso_unpack_hdr(a, &hdr);
    CHECK(logs_i == 4);                     /* banner, PSN, length, PS */
    logs_i = 0;
    ll_iso_unpack_hdr(b, &hdr);
    CHECK(logs_i == 5);                     /* banner, TS, PSN, length, PS */
    log_tag_const_i_LL_ISO = 0;

    /* hci_push_iso_sdu: complete SDU of 10 bytes for CIS handle 5. */
    u8 pkt[24] = { 5, 0x20, 14, 0, 0x01, 0x00, 10, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    cis.pduSizeMToS = 4; cis.bnMToS = 1; cis.bnSToM = 1; cis.role = 0; cis.framing = 0;
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    null_pdu_area[0] = 0xa5; null_pdu_area[1] = 0xff;
    hci_push_iso_sdu(pkt, 18);
    CHECK(cis_handle_seen == 5);
    /* fragments 4, 4, 2: header low byte keeps (0xa5 & 0xfc) = 0xa4, high byte = last fragment length 2, more = 0. */
    CHECK(null_pdu_area[0] == 0xa4 && null_pdu_area[1] == 2);
    CHECK(null_pdu_area[2] == 8 && null_pdu_area[3] == 9 && null_pdu_area[4] == 6 && null_pdu_area[5] == 7 && null_pdu_area[6] == 0xee);

    /* A single fragment sets the length with more = 0; a second one before it sets more = 1 in the running header. */
    pkt[6] = 3;
    null_pdu_area[0] = 0x03; null_pdu_area[1] = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(null_pdu_area[0] == 0x00 && null_pdu_area[1] == 3 && null_pdu_area[2] == 0 && null_pdu_area[3] == 1 && null_pdu_area[4] == 2);
    pkt[6] = 0;                             /* zero-length SDU still produces one empty fragment */
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    hci_push_iso_sdu(pkt, 18);
    CHECK(null_pdu_area[0] == (0xee & 0xfc) && null_pdu_area[1] == 0 && null_pdu_area[2] == 0xee);
    pkt[6] = 10;

    /* Rejections. */
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    log_tag_const_e_LL_ISO = 1;
    cis_established = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 1 && null_pdu_area[0] == 0xee);
    cis_established = 1;
    cis.bnMToS = 0;                         /* master with bnMToS 0 is refused; as a slave it is fine */
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 2 && null_pdu_area[0] == 0xee);
    cis.role = 1;
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 2 && null_pdu_area[0] != 0xee);
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    cis.bnSToM = 0;                         /* slave with bnSToM 0 is refused */
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 3 && null_pdu_area[0] == 0xee);
    cis.role = 0; cis.bnMToS = 1;
    hci_push_iso_sdu(pkt, 18);              /* master with bnSToM 0 is fine */
    CHECK(logs_e == 3 && null_pdu_area[0] != 0xee);
    memset(null_pdu_area, 0xee, sizeof(null_pdu_area));
    cis.framing = 1;                        /* framed: nothing is sent */
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_e == 3 && null_pdu_area[0] == 0xee);
    log_tag_const_e_LL_ISO = 0;

    /* No CIS: the BIS lookup is made (and ignored); the handle is masked to 12 bits. */
    pkt[0] = 7; pkt[1] = 0x20;
    bis_calls = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(bis_calls == 1 && bis_handle == 7 && cis_handle_seen == 7);
    pkt[0] = 0xff; pkt[1] = 0x2f;
    hci_push_iso_sdu(pkt, 18);
    CHECK(bis_handle == 0xfff);
    log_tag_const_i_LL_ISO = 1;
    logs_i = 0;
    hci_push_iso_sdu(pkt, 18);
    CHECK(logs_i == 5 && printf_buf_len == 10);    /* unpack: banner, PSN, length, PS; push: "ISO SDU" */
    log_tag_const_i_LL_ISO = 0;

    /* Codec send. */
    ll_iso_rt_cfg.maxIsoSduLen = 100;
    ll_codec.in = codec_in;
    big.maxSdu = 77;
    bis.big = &big;
    codec_result = 0;
    log_tag_const_d_LL_ISO = 1;
    lctrIsoSendCodecSdu(9);                 /* unknown handle: debug log */
    CHECK(logs_d == 1 && malloc_size == 0);
    log_tag_const_d_LL_ISO = 0;
    free_count = 0;
    lctrIsoSendCodecSdu(7);
    CHECK(malloc_size == 108 && codec_arg_handle == 7 && codec_arg_buf == (u8 *)malloc_ptr + 12 && codec_arg_len == 77);
    CHECK(codec_arg_ctr != NULL && free_count == 1 && freed_ptr == malloc_ptr);       /* no data: buffer freed */
    codec_result = 5;
    free_count = 0;
    lctrIsoSendCodecSdu(7);
    CHECK(free_count == 0);                 /* data available: the SDK leaks the buffer */
    malloc_fail = 1;
    log_tag_const_i_LL_ISO = 1;
    logs_i = 0;
    codec_arg_handle = 0;
    lctrIsoSendCodecSdu(7);
    CHECK(logs_i == 1 && codec_arg_handle == 0 && free_count == 0);
    malloc_fail = 0;
    ll_iso_rt_cfg.maxIsoSduLen = 0xffff;    /* the size is a 32-bit sum */
    lctrIsoSendCodecSdu(7);
    CHECK(malloc_size == 0xffff + 8);

    /* Lifecycle: the constants and the four codec callbacks are the BD19 IR
     * stores at ll_iso_init lines 899-931. */
    test_transport.send_packet = send_packet;
    memset(&hci_param, 0, sizeof(hci_param));
    memset(&le_param, 0, sizeof(le_param));
    memset(&ll_iso_hdl, 0, sizeof(ll_iso_hdl));
    memset(&ll_codec, 0, sizeof(ll_codec));
    ll_iso_init();
    CHECK(hci_param.hci_version == 11 && hci_param.lmp_pal_version == 11);
    CHECK(hci_param.event_mask[0] == 0x94 && hci_param.event_mask[1] == 0x8c &&
          hci_param.event_mask[4] == 0x00 && hci_param.event_mask[7] == 0x20);
    CHECK(le_param.event_mask[0] == 0xff && le_param.event_mask[1] == 0xff &&
          le_param.event_mask[2] == 0x8f && le_param.event_mask[3] == 0xff &&
          le_param.event_mask[4] == 0x03 && le_param.event_mask[7] == 0);
    CHECK(ll_iso_hdl.maxIsoPduLen == 251 && ll_iso_hdl.max_cis_number == 0);
    CHECK(ll_codec.start == PalCodecDataStartStream && ll_codec.stop == PalCodecDataStopStream);
    CHECK(ll_codec.in == PalCodecDataStreamIn && ll_codec.out == PalCodecDataStreamOut);
    CHECK(bis_init_calls == 1 && cis_init_calls == 1 && iso_queue_init_calls == 1);
    CHECK(thread_create_calls == 1 && created_thread == &ll_iso_hdl.thread &&
          created_entry == ll_iso_process && !strcmp(created_name, "isoal"));
    ll_iso_destroy();
    CHECK(bis_close_calls == 1 && cis_close_calls == 1 && iso_queue_destroy_calls == 1);

    /* ll_bis_process walks the BIS list and returns when it reaches the head. */
    memset(&ll_iso_hdl, 0, sizeof(ll_iso_hdl));
    ll_iso_hdl.bis_head.next = &ll_iso_hdl.bis_head;
    ll_bis_process();
    bis.entry.prev = &ll_iso_hdl.bis_head;
    bis.entry.next = &ll_iso_hdl.bis_head;
    ll_iso_hdl.bis_head.next = &bis.entry;
    ll_iso_hdl.bis_head.prev = &bis.entry;
    ll_bis_process();

    /* ll_iso_process drains messages, dispatches the non-255 state, frees each
     * message, records the elapsed high-water mark, then suspends. */
    memset(iso_messages, 0, sizeof(iso_messages));
    memset(ll_dispatch_msg_table, 0, sizeof(ll_dispatch_msg_table));
    ll_dispatch_msg_table[3] = dispatch_message;
    iso_messages[0].state_id = 3;
    iso_messages[0].msg_id = 0x44;
    iso_messages[0].payload[0] = 0x34;
    iso_messages[0].payload[1] = 0x12;
    iso_messages[1].state_id = 255;
    iso_message_count = 2;
    iso_message_head = iso_message_free_calls = dispatch_calls = 0;
    timer_values[0] = 100;
    timer_values[1] = 109;
    timer_count = 2;
    timer_index = 0;
    struct thread process_thread;
    created_thread = &process_thread;
    thread_suspend_calls = 0;
    ll_iso_process(&process_thread);
    CHECK(dispatch_calls == 1 && dispatch_state == 0x44 && dispatch_payload == 0x1234);
    CHECK(iso_message_free_calls == 2 && thread_suspend_calls == 1);
    CHECK(ll_iso_process_time_water_mark == 0xffffffffu); /* original initial maximum */
    ll_iso_process_time_water_mark = 0;
    timer_index = 0; iso_message_head = iso_message_count;
    ll_iso_process(&process_thread); CHECK(ll_iso_process_time_water_mark == 9);
    ll_iso_process_time_water_mark = 0;
    timer_values[0] = 0xfffffffeu; timer_values[1] = 3; timer_index = 0;
    ll_iso_process(&process_thread); CHECK(ll_iso_process_time_water_mark == 5);

    /* HCI LE ISO commands, from IR lines 715-806. */
    memset(&cis, 0, sizeof(cis));
    memset(&bis, 0, sizeof(bis));
    memset(&big, 0, sizeof(big));
    bis.big = &big;
    u8 command[20] = {0};
    event_calls = 0;
    le_param.acl_data_packet_length = 27;
    le_param.acl_packets_total_num = 3;
    le_param.iso_data_packet_length = 64;
    le_param.iso_packets_total_num = 2;
    CHECK(le_cmd_set_iso(96, command, sizeof(command)) == 1);
    CHECK(event_calls == 1 && event_opcode == 96 && !strcmp(event_format, "12121"));
    CHECK(event_args[0] == 0 && event_args[1] == 27 && event_args[2] == 3 &&
          event_args[3] == 64 && event_args[4] == 2);

    cis.cisEvtCounter = 0x1234;
    command[0] = 5;
    CHECK(le_cmd_set_iso(97, command, 2) == 1);
    CHECK(event_opcode == 97 && !strcmp(event_format, "12243"));
    CHECK(event_args[0] == 0 && event_args[1] == 5 && event_args[2] == 0x1234 &&
          event_args[3] == -1 && event_args[4] == 0);
    big.bn = 3;
    big.eventCounter = 0x10001;
    command[0] = 7;
    CHECK(le_cmd_set_iso(97, command, 2) == 1);
    CHECK(event_args[0] == 0 && event_args[1] == 7 && event_args[2] == 3);

    memset(command, 0, sizeof(command));
    command[0] = 5;
    command[2] = 0;
    command[3] = 1;
    cis.dataPathInCtx.id = LL_ISO_DATA_PATH_DISABLED;
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_DISABLED;
    CHECK(le_cmd_set_iso(110, command, sizeof(command)) == 1);
    CHECK(cis.dataPathInCtx.id == LL_ISO_DATA_PATH_VS);
    CHECK(!strcmp(event_format, "12") && event_args[0] == 0 && event_args[1] == 5);
    command[2] = 1;
    command[3] = 0;
    cis.dataPathOutCtx.cfg.hci.numRxPend = 9;
    CHECK(le_cmd_set_iso(110, command, sizeof(command)) == 1);
    CHECK(cis.dataPathOutCtx.id == LL_ISO_DATA_PATH_HCI &&
          cis.dataPathOutCtx.cfg.hci.numRxPend == 0 && event_args[0] == 0);
    command[0] = 7;
    command[2] = 0;
    command[3] = 2;
    big.role = LL_ROLE_SLAVE;
    CHECK(le_cmd_set_iso(110, command, sizeof(command)) == 1);
    CHECK(setup_path_calls == 1 && setup_path_direction == 0 && setup_path_id == 2);
    CHECK(event_args[0] == setup_path_result && event_args[1] == 7);

    command[2] = 2;
    CHECK(le_cmd_set_iso(110, command, sizeof(command)) == 1);
    CHECK(event_args[0] == 0 && setup_path_calls == 1);

    cis.dataPathInCtx.id = 0;
    cis.dataPathOutCtx.id = 0;
    command[0] = 5;
    CHECK(le_cmd_set_iso(111, command, 2) == 1);
    CHECK(cis.dataPathInCtx.id == LL_ISO_DATA_PATH_DISABLED &&
          cis.dataPathOutCtx.id == LL_ISO_DATA_PATH_DISABLED && event_args[0] == 0);
    CHECK(le_cmd_set_iso(111, command, 2) == 1);
    CHECK(event_args[0] == 12);             /* already disabled */

    command[0] = 5;
    command[2] = 3;
    CHECK(le_cmd_set_iso(112, command, 3) == 1 && event_args[0] == cis_tx_result);
    command[0] = 5;
    CHECK(le_cmd_set_iso(113, command, 3) == 1 && event_args[0] == cis_rx_result);
    command[0] = 7;
    CHECK(le_cmd_set_iso(112, command, 3) == 1 && event_args[0] == bis_tx_result);
    CHECK(le_cmd_set_iso(113, command, 3) == 1 && event_args[0] == bis_rx_result);
    cis_test_calls = bis_test_calls = 0;

    counter_result.numSuccess = 0x11;
    counter_result.numMissed = 0x22;
    counter_result.numFailed = 0x33;
    command[0] = 5;
    CHECK(le_cmd_set_iso(114, command, 2) == 1);
    CHECK(event_args[0] == 0 && event_args[2] == 0x11 &&
          event_args[3] == 0x22 && event_args[4] == 0x33);
    command[0] = 7;
    CHECK(le_cmd_set_iso(114, command, 2) == 1 && event_args[0] == 0);
    command[0] = 5;
    cis.txTestEnabled = 1;
    cis.rxTestEnabled = 1;
    counter_calls = 0;
    CHECK(le_cmd_set_iso(115, command, 2) == 1);
    CHECK(cis.txTestEnabled == 0 && cis.rxTestEnabled == 0 && counter_calls == 1);
    cis.txTestEnabled = 1; cis.rxTestEnabled = 0; counter_calls = 0;
    CHECK(le_cmd_set_iso(115, command, 2) == 1);
    CHECK(cis.txTestEnabled == 0 && counter_calls == 0);
    cis.txTestEnabled = 0; cis.rxTestEnabled = 1;
    CHECK(le_cmd_set_iso(115, command, 2) == 1);
    CHECK(cis.rxTestEnabled == 0 && counter_calls == 1);
    command[0] = 7;
    CHECK(le_cmd_set_iso(115, command, 2) == 1 && bis.test.term == 1);

    cis.isoLinkQualStats.txUnAckPkt = 1;
    cis.isoLinkQualStats.txFlushedPkt = 2;
    cis.isoLinkQualStats.txLastSubEventPkt = 3;
    cis.isoLinkQualStats.retransmitPkt = 4;
    cis.isoLinkQualStats.crcErrPkt = 5;
    cis.isoLinkQualStats.rxUnreceivedPkt = 6;
    cis.isoLinkQualStats.duplicatePkt = 7;
    command[0] = 5;
    CHECK(le_cmd_set_iso(117, command, 2) == 1 && !strcmp(event_format, "124444444"));
    CHECK(event_args[0] == 0 && event_args[2] == 1 && event_args[3] == 2 &&
          event_args[4] == 3 && event_args[5] == 4 && event_args[6] == 5 &&
          event_args[7] == 6 && event_args[8] == 7);
    bis.crcErrPkt = 0xabc;
    bis.rxUnreceivedPkt = 0xdef;
    command[0] = 7;
    CHECK(le_cmd_set_iso(117, command, 2) == 1);
    CHECK(event_args[6] == 0xabc && event_args[7] == 0xdef);
    CHECK(le_cmd_set_iso(0xffff, command, 2) == 0);

    /* Reassembly: an unframed start (five-byte header) followed by a
     * continuation/completion (two-byte header).  The expected HCI values
     * follow IR lines 1370-1391: load length = payload + 8, timestamp ff,
     * PB_COMP, and the accumulated five-byte unframed payload. */
    memset(&cis, 0, sizeof(cis));
    cis.cisHandle = 0xabc;
    cis.sduSizeMToS = 64;
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    ll_iso_rt_cfg.maxIsoSduLen = 64;
    free_count = 0;
    send_calls = 0;
    u8 reassembly_pdu[32] = {0};
    cis_pdu_t *rpdu = (cis_pdu_t *)reassembly_pdu;
    rpdu->llid = 2;
    rpdu->length = 8;
    rpdu->payload[0] = 0x00;                /* start, not complete */
    rpdu->payload[1] = 6;                   /* encoded payload length + 3 */
    rpdu->payload[2] = 0x34;
    rpdu->payload[3] = 0x12;
    rpdu->payload[4] = 0x00;
    rpdu->payload[5] = 0xaa;
    rpdu->payload[6] = 0xbb;
    rpdu->payload[7] = 0xcc;
    ll_iso_cis_sdu_reassembly(&cis, rpdu);
    CHECK(cis.isoalRxCtx.rxState == 1 && cis.isoalRxCtx.rxSduOffset == 15);
    CHECK(send_calls == 0 && cis.isoalRxCtx.data.framed.timeOffset == 0x1234);
    CHECK(cis.isoalRxCtx.pPendSduBuf[12] == 0xaa &&
          cis.isoalRxCtx.pPendSduBuf[14] == 0xcc);

    rpdu->length = 4;
    rpdu->payload[0] = 0x03;                /* continuation, complete */
    rpdu->payload[1] = 2;
    rpdu->payload[2] = 0xdd;
    rpdu->payload[3] = 0xee;
    ll_iso_cis_sdu_reassembly(&cis, rpdu);
    CHECK(cis.isoalRxCtx.rxState == 0 && cis.isoalRxCtx.rxSduOffset == 17);
    CHECK(send_calls == 1 && sent_type == 2 && sent_len == 17);
    CHECK(sent_packet[0] == 0xbc && sent_packet[1] == 0x6a &&
          sent_packet[2] == 13 && sent_packet[3] == 0);
    CHECK(sent_packet[4] == 0xff && sent_packet[5] == 0 &&
          sent_packet[6] == 0 && sent_packet[7] == 0);
    CHECK(sent_packet[10] == 5);
    CHECK(!memcmp(sent_packet + 12, "\xaa\xbb\xcc\xdd\xee", 5));
    free(cis.isoalRxCtx.pPendSduBuf);
    cis.isoalRxCtx.pPendSduBuf = NULL;

    memset(&cis.isoalRxCtx, 0, sizeof(cis.isoalRxCtx));
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    rpdu->length = 4;
    rpdu->payload[0] = 0x01;                /* continuation without a start */
    rpdu->payload[1] = 2;
    rpdu->payload[2] = 1;
    rpdu->payload[3] = 2;
    free_count = 0;
    send_calls = 0;
    ll_iso_cis_sdu_reassembly(&cis, rpdu);
    CHECK(cis.isoalRxCtx.rxState == 0 && cis.isoalRxCtx.pPendSduBuf == NULL);
    CHECK(send_calls == 0 && free_count == 1);

    /* Recombination: FIRST and LAST PDUs are queued, then the completed SDU
     * is emitted to HCI.  IR lines 1091-1177 give offsets 12 and 4 and send
     * sizes of the SDK load length plus four header bytes. */
    memset(&cis, 0, sizeof(cis));
    cis.cisHandle = 0x321;
    cis.sduSizeMToS = 64;
    cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    u8 recombine_queue_memory[64];
    cis.isoalRxCtx.data.unframed.pendSduQ =
        (struct lbuff_head *)lbuf_init_btctrler(recombine_queue_memory,
                                                sizeof(recombine_queue_memory));
    lbuf_queue_count = lbuf_push_calls = lbuf_pop_calls = lbuf_free_calls = 0;
    send_calls = 0;
    u8 recombine_storage[32] = {0};
    u8 recombine_storage2[32] = {0};
    cis_pdu_t *cpdu = (cis_pdu_t *)(recombine_storage + 12);
    cpdu->llid = 1;                         /* FIRST */
    cpdu->length = 3;
    cpdu->payload[0] = 0x01;
    cpdu->payload[1] = 0x02;
    cpdu->payload[2] = 0x03;
    ll_iso_cis_sdu_recombination(&cis, cpdu);
    CHECK(cis.isoalRxCtx.rxState == 1 && cis.isoalRxCtx.data.unframed.curLen == 3);
    CHECK(lbuf_push_calls == 1 && send_calls == 0);
    cis_pdu_t *cpdu2 = (cis_pdu_t *)(recombine_storage2 + 12);
    cpdu2->llid = 0;                        /* LAST */
    cpdu2->length = 2;
    cpdu2->payload[0] = 0x04;
    cpdu2->payload[1] = 0x05;
    ll_iso_cis_sdu_recombination(&cis, cpdu2);
    CHECK(cis.isoalRxCtx.rxState == 0 && cis.isoalRxCtx.packetSequence == 1);
    CHECK(lbuf_push_calls == 2 && lbuf_pop_calls == 2 && lbuf_free_calls == 2);
    CHECK(send_calls == 2 && sent_type == 2);
    CHECK(sent_lengths[0] == 15 && sent_packets[0][0] == 0x21 && sent_packets[0][1] == 0x43);
    CHECK(sent_lengths[1] == 6 && sent_packets[1][0] == 0x21 && sent_packets[1][1] == 0x33);

    ll_iso_cis_sdu_segmentation(&cis, recombine_storage, 3);  /* SDK body is empty */
    /* Multiple complete segments must subtract from the remaining PDU bytes. */
    u8 multi_storage[20] = {0}; cis_pdu_t *multi = (cis_pdu_t *)multi_storage;
    multi->length = 16; multi->llid = 2;
    static const u8 segments[] = {2,6,0x11,0x22,0x33,1,2,3, 2,6,0x44,0x55,0x66,4,5,6};
    memcpy(multi->payload, segments, sizeof(segments));
    memset(&cis.isoalRxCtx, 0, sizeof(cis.isoalRxCtx));
    cis.sduSizeMToS = 64; cis.dataPathOutCtx.id = LL_ISO_DATA_PATH_HCI;
    send_calls = 0; ll_iso_cis_sdu_reassembly(&cis, multi);
    CHECK(send_calls == 2 && cis.isoalRxCtx.packetSequence == 2);
    CHECK(cis.isoalRxCtx.data.framed.timeOffset == 0x332211); /* SDK uses initial PDU payload */
    CHECK(!memcmp(sent_packets[0] + 12, segments + 5, 3));
    CHECK(!memcmp(sent_packets[1] + 12, segments + 13, 3));
    free(cis.isoalRxCtx.pPendSduBuf); cis.isoalRxCtx.pPendSduBuf = NULL;

    segm_hdr_t segment = {0}; u8 segment_packet[8] = {0};
    segment.sc = 1; segment.length = 2; log_tag_const_i_LL_ISO = 1;
    mutate_segment = &segment;
    CHECK(ll_iso_pack_seg_hdr(segment_packet, &segment) == 5);
    CHECK(segment_packet[0] == 1 && segment_packet[1] == 10);
    CHECK(segment_packet[2] == 0x11 && segment_packet[3] == 0x22 && segment_packet[4] == 0x33);
    hci_iso_hdr_t changed = {0}; u8 changed_packet[24] = {0};
    changed.handle = 0x123; changed.pb_flag = 1; changed.iso_data_load_length = 5;
    mutate_header = &changed;
    CHECK(ll_iso_pack_hdr(changed_packet, &changed) == 12);
    CHECK(changed_packet[0] == 0x23 && changed_packet[1] == 0x11 && changed_packet[2] == 13);
    CHECK(changed_packet[4] == 0xdd && changed_packet[7] == 0xaa && changed_packet[8] == 0x22);
    log_tag_const_i_LL_ISO = 0;
    hci_iso_hdr_t overlapping = {0}; overlapping.handle = 5; overlapping.iso_data_load_length = 255;
    CHECK(ll_iso_pack_hdr((u8 *)&overlapping, &overlapping) == 8);
    CHECK(((u8 *)&overlapping)[2] == 3 && ((u8 *)&overlapping)[3] == 0);
    CHECK(le_cmd_set_iso(0x2060, command, 0) == 1 && event_opcode == 0x2060);
    return TEST_DONE("btctrler_ll_common_iso");
}
#endif
