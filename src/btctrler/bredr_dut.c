/* The SDK's Classic device-under-test mode (bredr_dut.c): when a DUT
 * scenario is active, received ACL payloads are looped back (optionally with
 * PRBS9 checking) or test data is transmitted, the RF is configured per
 * scenario, and the link's transmit buffers are cleared at the end.  The
 * receive handler also does the link's normal Classic receive of LMP packets
 * and drives the frame transmit slots (the SDK's dut_tx_handler).
 *
 * SDK quirks kept: dut_rx_handler pushes the LMP receive bulk even when its
 * allocation failed, and the loop-back path copies from a bulk that may be
 * null; when the transmit slot's previous bulk is not finished it is
 * overwritten by the next pop (not freed); the read of g_dut_hdl->start is
 * not guarded against a null g_dut_hdl; bredr_dut_detch clears g_dut_hdl
 * without freeing it.  dut_rx_handler's receive-slot loop ends when the other
 * slot's whole control byte (not its bit 0) is zero, and rx_full carries
 * over to the next slot.  The two classic-baseband
 * registers at 0x1c0000 and 0x1c00d0 are written read-modify-write, in the
 * SDK's order. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

void log_print(int level, const char *file, const char *format, ...);
int putchar(int c);
#ifdef UNITTEST
extern u8 log_tag_const_i_BL;
extern int CONFIG_TEST_DUT_ONLY_BOX_CODE;
#else
extern const u8 log_tag_const_i_BL;
extern const int CONFIG_TEST_DUT_ONLY_BOX_CODE;
#endif

#define DUT_CODE __attribute__((section(".classic_rf_code")))
#define DUT_CONST __attribute__((section(".classic_rf_const")))

#ifdef UNITTEST
extern volatile u32 dut_reg_c0000, dut_reg_c00d0;
#define BB_REG_A dut_reg_c0000
#define BB_REG_B dut_reg_c00d0
#else
#define BB_REG_A (*(volatile u32 *)(uintptr_t)0x1c0000)
#define BB_REG_B (*(volatile u32 *)(uintptr_t)0x1c00d0)
#endif

#define LMP_EVENT_RX 5

static const u16 iut_aclsco_table[10][2] DUT_CONST = {
    { 10, 8 }, { 10, 5 }, { 17, 3 }, { 20, 6 }, { 27, 4 }, { 30, 7 }, { 121, 10 }, { 183, 11 },
    { 224, 14 }, { 339, 15 },
};
static const u16 iut_esco_table[3][2] DUT_CONST = { { 30, 7 }, { 120, 12 }, { 180, 13 } };
static const u16 iut_edracl_table[6][2] DUT_CONST = {
    { 54, 4 }, { 83, 8 }, { 367, 10 }, { 552, 11 }, { 679, 14 }, { 1021, 15 },
};
static const u16 iut_edresco_table[4][2] DUT_CONST = { { 60, 6 }, { 360, 7 }, { 90, 12 }, { 540, 13 } };
static const u8 value_table[10] = { 0x00, 0x00, 0x01, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f };

/* The PRBS9 sequence (x^9 + x^5 + 1) as bytes, one 511-byte period, and the
 * SDK's position table into it. */
const u16 prbs9_table0[511] __attribute__((section(".classic_rf_const"), aligned(2))) = {
    144, 80, 224, 16, 304, 160, 180, 260, 463, 240, 298, 96,
    173, 116, 384, 399, 464, 185, 196, 176, 216, 234, 253, 32,
    82, 109, 340, 52, 378, 320, 510, 209, 400, 79, 335, 121,
    458, 132, 279, 152, 162, 112, 184, 189, 454, 170, 420, 18,
    33, 334, 479, 276, 357, 45, 265, 314, 331, 333, 499, 296,
    446, 194, 256, 336, 452, 145, 274, 271, 376, 14, 15, 57,
    411, 118, 394, 68, 219, 215, 413, 88, 113, 98, 261, 48,
    202, 120, 414, 35, 125, 345, 390, 106, 437, 22, 356, 441,
    480, 289, 465, 159, 270, 387, 415, 293, 359, 212, 486, 201,
    252, 27, 492, 267, 500, 83, 250, 131, 269, 23, 435, 232,
    327, 242, 382, 130, 264, 192, 221, 272, 432, 301, 388, 81,
    344, 210, 506, 207, 407, 37, 312, 393, 462, 322, 461, 69,
    504, 28, 347, 241, 330, 54, 163, 4, 417, 103, 155, 151,
    211, 349, 370, 49, 472, 10, 24, 197, 369, 34, 339, 138,
    467, 367, 495, 350, 460, 56, 239, 107, 482, 61, 222, 110,
    281, 326, 332, 230, 373, 42, 439, 55, 469, 111, 292, 416,
    490, 21, 377, 354, 401, 12, 225, 94, 206, 95, 214, 351,
    444, 323, 456, 229, 493, 220, 295, 148, 153, 299, 422, 137,
    204, 188, 491, 198, 428, 474, 507, 436, 509, 102, 203, 6,
    19, 186, 245, 67, 115, 205, 311, 236, 470, 371, 425, 193,
    263, 168, 438, 178, 315, 318, 341, 169, 200, 66, 494, 157,
    282, 128, 164, 208, 288, 244, 368, 324, 362, 237, 448, 17,
    249, 280, 317, 146, 404, 63, 442, 143, 273, 11, 343, 226,
    248, 7, 484, 97, 398, 329, 421, 395, 397, 258, 360, 5,
    338, 78, 440, 182, 475, 283, 477, 177, 325, 266, 478, 99,
    409, 86, 501, 353, 505, 223, 451, 39, 423, 91, 316, 53,
    147, 87, 195, 306, 391, 285, 328, 365, 496, 59, 408, 101,
    471, 386, 457, 92, 133, 227, 305, 167, 481, 275, 434, 25,
    74, 403, 433, 20, 431, 13, 303, 171, 286, 174, 396, 294,
    503, 119, 175, 43, 85, 76, 418, 158, 278, 9, 508, 46,
    284, 217, 363, 44, 268, 60, 262, 62, 166, 70, 309, 179,
    375, 300, 489, 257, 502, 379, 405, 47, 233, 228, 346, 308,
    352, 1, 426, 313, 381, 127, 468, 75, 337, 71, 290, 161,
    485, 424, 459, 142, 402, 30, 246, 31, 389, 150, 473, 58,
    287, 380, 487, 117, 259, 392, 455, 123, 429, 165, 450, 156,
    291, 231, 498, 89, 497, 77, 84, 235, 238, 183, 358, 140,
    149, 73, 342, 348, 427, 108, 124, 126, 134, 243, 364, 321,
    443, 297, 410, 65, 372, 191, 445, 135, 139, 38, 488, 310,
    466, 26, 453, 40, 122, 8, 181, 3, 187, 51, 355, 50,
    141, 247, 302, 213, 406, 172, 412, 190, 307, 361, 385, 129,
    255, 41, 199, 90, 374, 72, 104, 251, 419, 114, 366, 277,
    476, 254, 449, 105, 319, 136, 154, 430, 483, 2, 29, 218,
    383, 36, 93, 100, 447, 0, 64,
};

const u8 prbs9_table1[511] __attribute__((section(".classic_rf_const"), aligned(1))) = {
    0xff, 0xc1, 0xfb, 0xe8, 0x4c, 0x90, 0x72, 0x8b, 0xe7, 0xb3, 0x51, 0x89,
    0x63, 0xab, 0x23, 0x23, 0x02, 0x84, 0x18, 0x72, 0xaa, 0x61, 0x2f, 0x3b,
    0x51, 0xa8, 0xe5, 0x37, 0x49, 0xfb, 0xc9, 0xca, 0x0c, 0x18, 0x53, 0x2c,
    0xfd, 0x45, 0xe3, 0x9a, 0xe6, 0xf1, 0x5d, 0xb0, 0xb6, 0x1b, 0xb4, 0xbe,
    0x2a, 0x50, 0xea, 0xe9, 0x0e, 0x9c, 0x4b, 0x5e, 0x57, 0x24, 0xcc, 0xa1,
    0xb7, 0x59, 0xb8, 0x87, 0xff, 0xe0, 0x7d, 0x74, 0x26, 0x48, 0xb9, 0xc5,
    0xf3, 0xd9, 0xa8, 0xc4, 0xb1, 0xd5, 0x91, 0x11, 0x01, 0x42, 0x0c, 0x39,
    0xd5, 0xb0, 0x97, 0x9d, 0x28, 0xd4, 0xf2, 0x9b, 0xa4, 0xfd, 0x64, 0x65,
    0x06, 0x8c, 0x29, 0x96, 0xfe, 0xa2, 0x71, 0x4d, 0xf3, 0xf8, 0x2e, 0x58,
    0xdb, 0x0d, 0x5a, 0x5f, 0x15, 0x28, 0xf5, 0x74, 0x07, 0xce, 0x25, 0xaf,
    0x2b, 0x12, 0xe6, 0xd0, 0xdb, 0x2c, 0xdc, 0xc3, 0x7f, 0xf0, 0x3e, 0x3a,
    0x13, 0xa4, 0xdc, 0xe2, 0xf9, 0x6c, 0x54, 0xe2, 0xd8, 0xea, 0xc8, 0x88,
    0x00, 0x21, 0x86, 0x9c, 0x6a, 0xd8, 0xcb, 0x4e, 0x14, 0x6a, 0xf9, 0x4d,
    0xd2, 0x7e, 0xb2, 0x32, 0x03, 0xc6, 0x14, 0x4b, 0x7f, 0xd1, 0xb8, 0xa6,
    0x79, 0x7c, 0x17, 0xac, 0xed, 0x06, 0xad, 0xaf, 0x0a, 0x94, 0x7a, 0xba,
    0x03, 0xe7, 0x92, 0xd7, 0x15, 0x09, 0x73, 0xe8, 0x6d, 0x16, 0xee, 0xe1,
    0x3f, 0x78, 0x1f, 0x9d, 0x09, 0x52, 0x6e, 0xf1, 0x7c, 0x36, 0x2a, 0x71,
    0x6c, 0x75, 0x64, 0x44, 0x80, 0x10, 0x43, 0x4e, 0x35, 0xec, 0x65, 0x27,
    0x0a, 0xb5, 0xfc, 0x26, 0x69, 0x3f, 0x59, 0x99, 0x01, 0x63, 0x8a, 0xa5,
    0xbf, 0x68, 0x5c, 0xd3, 0x3c, 0xbe, 0x0b, 0xd6, 0x76, 0x83, 0xd6, 0x57,
    0x05, 0x4a, 0x3d, 0xdd, 0x81, 0x73, 0xc9, 0xeb, 0x8a, 0x84, 0x39, 0xf4,
    0x36, 0x0b, 0xf7, 0xf0, 0x1f, 0xbc, 0x8f, 0xce, 0x04, 0x29, 0xb7, 0x78,
    0x3e, 0x1b, 0x95, 0x38, 0xb6, 0x3a, 0x32, 0x22, 0x40, 0x88, 0x21, 0xa7,
    0x1a, 0xf6, 0xb2, 0x13, 0x85, 0x5a, 0x7e, 0x93, 0xb4, 0x9f, 0xac, 0xcc,
    0x80, 0x31, 0xc5, 0xd2, 0x5f, 0x34, 0xae, 0x69, 0x1e, 0xdf, 0x05, 0x6b,
    0xbb, 0x41, 0xeb, 0xab, 0x02, 0xa5, 0x9e, 0xee, 0xc0, 0xb9, 0xe4, 0x75,
    0x45, 0xc2, 0x1c, 0x7a, 0x9b, 0x85, 0x7b, 0xf8, 0x0f, 0xde, 0x47, 0x67,
    0x82, 0x94, 0x5b, 0x3c, 0x9f, 0x8d, 0x4a, 0x1c, 0x5b, 0x1d, 0x19, 0x11,
    0x20, 0xc4, 0x90, 0x53, 0x0d, 0x7b, 0xd9, 0x89, 0x42, 0x2d, 0xbf, 0x49,
    0xda, 0x4f, 0x56, 0x66, 0xc0, 0x98, 0x62, 0xe9, 0x2f, 0x1a, 0xd7, 0x34,
    0x8f, 0xef, 0x82, 0xb5, 0xdd, 0xa0, 0xf5, 0x55, 0x81, 0x52, 0x4f, 0x77,
    0xe0, 0x5c, 0xf2, 0xba, 0x22, 0x61, 0x0e, 0xbd, 0xcd, 0xc2, 0x3d, 0xfc,
    0x07, 0xef, 0xa3, 0x33, 0x41, 0xca, 0x2d, 0x9e, 0xcf, 0x46, 0x25, 0x8e,
    0xad, 0x8e, 0x8c, 0x08, 0x10, 0x62, 0xc8, 0xa9, 0x86, 0xbd, 0xec, 0x44,
    0xa1, 0x96, 0xdf, 0x24, 0xed, 0x27, 0x2b, 0x33, 0x60, 0x4c, 0xb1, 0xf4,
    0x17, 0x8d, 0x6b, 0x9a, 0xc7, 0x77, 0xc1, 0xda, 0x6e, 0xd0, 0xfa, 0xaa,
    0x40, 0xa9, 0xa7, 0x3b, 0x70, 0x2e, 0x79, 0x5d, 0x91, 0x30, 0x87, 0xde,
    0x66, 0xe1, 0x1e, 0xfe, 0x83, 0xf7, 0xd1, 0x99, 0x20, 0xe5, 0x16, 0xcf,
    0x67, 0xa3, 0x12, 0xc7, 0x56, 0x47, 0x46, 0x04, 0x08, 0x31, 0xe4, 0x54,
    0xc3, 0x5e, 0x76, 0xa2, 0x50, 0xcb, 0x6f, 0x92, 0xf6, 0x93, 0x95, 0x19,
    0x30, 0xa6, 0x58, 0xfa, 0x8b, 0xc6, 0x35, 0xcd, 0xe3, 0xbb, 0x60, 0x6d,
    0x37, 0x68, 0x7d, 0x55, 0xa0, 0xd4, 0xd3, 0x1d, 0x38, 0x97, 0xbc, 0xae,
    0x48, 0x98, 0x43, 0x6f, 0xb3, 0x70, 0x0f,
};

static int list_empty(struct list_head *head) { return head->next == head; }

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = prev;
    prev->next = entry;
}

static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

/* The (length, type) row for a packet type, per packet group: the last row
 * when none matches. */
DUT_CODE const u16 *dut_get_packet_max_len(int packet, u8 group)
{
    const u16 (*table)[2];
    int count;
    switch (group) {
    case 0: table = iut_aclsco_table; count = 10; break;
    case 2: table = iut_edracl_table; count = 6; break;
    case 3: table = iut_edresco_table; count = 4; break;
    default: table = iut_esco_table; count = 3; break;
    }
    int i;
    for (i = 0; i < count; i++)
        if (table[i][1] == packet) return table[i];
    return table[i - 1];
}

static DUT_CODE u8 cal_hop_fre(u8 fre)
{
    int half = fre >> 1;
    return (fre & 1) == 0 ? half : half + 40;
}

/* Configure the baseband for the scenario in g_dut_hdl. */
DUT_CODE void dut_cfg_analog(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    struct bredr_dut *dut = g_dut_hdl;
    if (log_tag_const_i_BL) log_print(2, (const char *)0, "[Info]: [BL]sc=%d\n\r\n", dut->scenario);
    BB_REG_B = BB_REG_B & ~1u;
    switch (dut->scenario) {
    case 0:
        dut->start = 0;
        return;
    case 4:
        BB_REG_B = BB_REG_B | 1;
        BB_REG_B = BB_REG_B & ~2u;
        /* fall through */
    case 1: case 2: case 3: case 9:
        BB_REG_A = BB_REG_A | 524288;
        dut->start = 1;
        break;
    case 5: case 6:
        BB_REG_A = BB_REG_A & ~524288u;
        dut->start = 1;
        break;
    case 7: case 8:
        BB_REG_A = BB_REG_A | 524288;
        dut->start = 1;
        break;
    case 255:
        BB_REG_A = BB_REG_A & ~524288u;
        dut->start = 0;
        break;
    default:
        break;
    }
    if (dut->hopping_mode == 0) {
        BB_REG_A = BB_REG_A | 65536;
        frame->format = frame->format & ~64;
        frame->rxfreq = cal_hop_fre(dut->rx_fre);
        frame->txfreq = cal_hop_fre(dut->tx_fre);
    } else {
        if (frame->miscntl > 63) frame->format = frame->format | 64;
        BB_REG_A = BB_REG_A & ~65536u;
    }
    dut->right_packet_cnt = 0;
    u8 packet_type = dut->packet_type;
    dut->packet_group = packet_type >> 4;
    dut->packet_num = packet_type & 15;
    u8 miscntl = frame->miscntl;
    frame->miscntl = packet_type > 31 ? (miscntl | 8) : (miscntl & ~8);
    if (CONFIG_TEST_DUT_ONLY_BOX_CODE == 0)
        dut->packet_len = *dut_get_packet_max_len(packet_type & 15, packet_type >> 4);
    else
        dut->packet_len = 339;
}

DUT_CODE void dut_clear_tx_buf(struct link_connection *lc)
{
    if (g_dut_hdl == 0) return;
    if (lc->test_con_flag != 0) return;
    struct ctrl_frame *frame = lc->frame;
    local_irq_disable();
    if (lc->tx_bulk[0] != 0) {
        bredr_tx_bulk_free(lc->tx_bulk[0]);
        if (lc->tx_bulk[1] == lc->tx_bulk[0]) lc->tx_bulk[1] = 0;
        lc->tx_bulk[0] = 0;
    }
    if (lc->tx_bulk[1] != 0) bredr_tx_bulk_free(lc->tx_bulk[1]);
    lc->tx_bulk[1] = 0;
    struct list_head *entry = lc->tx_acl_u.next;
    while (entry != &lc->tx_acl_u) {
        struct list_head *next = entry->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)entry - offsetof(struct bredr_tx_bulk, entry)));
        entry = next;
    }
    __write_reg_packet_type(frame, 0, 0);
    __write_reg_packet_type(frame, 1, 0);
    local_irq_enable();
}

DUT_CODE void bredr_dut_detch(struct link_connection *lc)
{
    struct bredr_dut *dut = g_dut_hdl;
    if (lc != 0 && dut != 0 && lc->frame != 0) {
        dut->scenario = 255;
        dut_cfg_analog(lc);
        BB_REG_B = BB_REG_B & ~1u;
        BB_REG_A = BB_REG_A & ~524288u;
        BB_REG_A = BB_REG_A & ~65536u;
        g_dut_hdl = 0;
    }
}

/* Fill the transmit bulk from the received one: a PRBS9 pattern is
 * regenerated from the position found in the first bytes when the received
 * payload looks like one (many distinct byte values); a repeating payload is
 * tiled; anything else is copied. */
static DUT_CODE void check_rx_fill_tx_data(struct bredr_rx_bulk *rx, struct bredr_tx_bulk *tx, u8 check,
                                           struct bredr_dut *dut)
{
    u8 same_table[50][2];
    u8 tmp_buf[16];
    memset(&same_table[0][1], 0, 99);
    same_table[0][0] = rx->data[0];
    u16 len = tx->len;
    int same_cnt = 1;
    int i, j;
    for (i = 0; i < (len < 48 ? len : 48); i++) {
        for (j = 0; j < same_cnt; j++) {
            if (same_table[j][0] == rx->data[i]) {
                same_table[j][1]++;
                break;
            }
        }
        if (j == same_cnt) {
            same_table[same_cnt][0] = rx->data[i];
            same_cnt++;
        }
        if (same_cnt > 15) {
            if ((check & 4) != 0) {
                memcpy(tx->data, rx->data, len);
                return;
            }
            u16 found_i = 0, found_pos = 0;
            for (i = 0; i < (len < 30 ? len : 30); i += 2) {
                u8 first = rx->data[i];
                for (int k = 0; k < 2; k++) {
                    u16 start = first == 0 ? first : (u16)((first << 1) + 65535);
                    u16 pos = prbs9_table0[k + start];
                    int matches = 0;
                    for (int m = 0; m < 3; m++) {
                        u16 at = (u16)((pos + 1) + m);
                        matches += prbs9_table1[at > 510 ? at - 511 : at] == rx->data[m + (i | 1)];
                    }
                    if (matches > 1) {
                        found_i = (u16)i;
                        found_pos = pos;
                        goto found;
                    }
                }
            }
            found_pos = 0;
        found:;
            u16 diff = (u16)(found_pos - found_i);
            int from = found_pos < found_i ? (u16)(diff + 511) : diff;
            int first_run = 511 - from;
            int done, remaining;
            if ((int)len > first_run) {
                memcpy(tx->data, prbs9_table1 + from, first_run);
                remaining = (u16)(len - first_run);
                done = (u16)first_run;
            } else {
                memcpy(tx->data, prbs9_table1 + from, len);
                done = 0;
                remaining = 0;
            }
            while (remaining != 0) {
                if (remaining < 512) {
                    memcpy(tx->data + done, prbs9_table1, remaining);
                    remaining = 0;
                } else {
                    memcpy(tx->data + done, prbs9_table1, 511);
                    done = (u16)(done + 511);
                    remaining = (u16)(remaining - 511);
                }
            }
            dut->right_packet_cnt++;
            pseudo_random_genrate(tmp_buf, 16);
            if ((tmp_buf[0] & 63) == 0 && dut->right_packet_cnt > 199) {
                u8 pick = tmp_buf[1];
                u16 txlen = tx->len;
                dut->right_packet_cnt = 0;
                tx->data[(u32)pick % (u32)txlen] ^= ((txlen > 299 ? 21 : 1) & pick);
            }
            return;
        }
    }
    for (i = 0; i < same_cnt; i++)
        if (same_table[i][1] > 19) break;
    if (same_table[i][0] == 0x55 || same_table[i][0] == 0) {
        u16 blocks = len / 48;
        for (i = 0; i < blocks; i++) memcpy(tx->data + i * 48, rx->data, 48);
        memcpy(tx->data + blocks * 48, rx->data, len % 48);
    } else {
        memcpy(tx->data, rx->data, len);
    }
}

/* Loop a received payload back as the next transmit packet. */
static void dut_loop_back(struct link_connection *lc, struct bredr_rx_bulk *rx, u8 check, int rx_len)
{
    struct bredr_dut *dut = g_dut_hdl;
    if (dut->start == 0) return;
    u8 scenario = dut->scenario;
    if (scenario == 0 || scenario == 255) return;
    if ((u32)scenario - 5 >= 4) return;
    int len = dut->packet_len < rx_len ? dut->packet_len : rx_len;
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(len);
    if (!tx) return;
    tx->packet = (u8)dut->packet_num;
    tx->offset = 0;
    tx->len = (u16)len;
    tx->event_post = 0;
    tx->llid = 2;
    if ((config_btctler_mode & 2) != 0) {
        check_rx_fill_tx_data(rx, tx, check, dut);
    } else if (rf.production_test == 0) {
        memcpy(tx->data, rx->data, len);
    } else if (dut->rx_fre == 0 || dut->tx_fre == 0) {
        check_rx_fill_tx_data(rx, tx, check, dut);
    } else {
        memcpy(tx->data, rx->data, len);
    }
    list_add_tail(&tx->entry, &lc->tx_acl_u);
}

/* Queue the scenario's test packet. */
static void dut_tx_mode(struct link_connection *lc)
{
    struct bredr_dut *dut = g_dut_hdl;
    if (dut == 0 || dut->start == 0) return;
    switch (dut->scenario) {
    case 9: case 4: case 1: case 2: case 3: {
        u16 len = dut->data_len == 0 ? dut->packet_len : dut->data_len;
        struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(len);
        if (!tx) return;
        tx->packet = (u8)dut->packet_num;
        tx->offset = 0;
        tx->len = len;
        tx->event_post = 0;
        tx->llid = 2;
        if (dut->scenario != 4) memset(tx->data, value_table[dut->scenario], len);
        list_add_tail(&tx->entry, &lc->tx_acl_u);
        break;
    }
    default:
        break;
    }
}

/* Move a bulk into transmit slot index of the frame. */
static void dut_link_ctrl_tx(struct link_connection *lc, struct bredr_tx_bulk *tx, int index)
{
    struct ctrl_frame *frame = lc->frame;
    u8 enc_prob = tx->enc_prob;
    if ((enc_prob & 4) != 0) lc_write_encry(lc, (enc_prob >> 1) & 1, enc_prob & 1);
    u8 event_prob = tx->event_prob;
    if (event_prob != 0) bredr_link_event(lc, 1, event_prob);
    __write_reg_txinfo(frame, index, tx->data, tx->len, tx->llid, 1, tx->packet);
    tx->offset = tx->len;
    list_del(&tx->entry);
    __clean_reg_txempty(frame, index);
}

/* The SDK's dut_tx_handler: complete finished transmit slots, then refill
 * them from the control queue or the DUT queue. */
static void dut_tx_handler(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    s8 update_index[2];
    update_index[0] = update_index[1] = -1;
    int index = frame->txtog & 1;
    for (int i = 0; i < 2; i++) {
        if (index == 0) {
            if ((frame->txbuf0cntl & 1) != 0 || (frame->linkcntl0 & 14) == 0) update_index[i] = 0;
            index = 1;
        } else {
            if ((frame->txbuf1cntl & 1) != 0 || (frame->linkcntl1 & 14) == 0) update_index[i] = (s8)index;
            index = 0;
        }
    }
    for (int i = 0; i < 2; i++) {
        int slot = update_index[i];
        if (slot == -1) continue;
        struct bredr_tx_bulk *tx = lc->tx_bulk[slot];
        if (tx != 0 && tx != lc->tx_bulk[slot == 0] && tx->offset == tx->len) {
            if (tx->dut_post) {
                dut_cfg_analog(lc);
                if (g_dut_hdl->scenario == 255) {
                    hci_send_event(250, "1", 0);
                    lmp_free(g_dut_hdl);
                    g_dut_hdl = 0;
                }
            }
            if (tx->llid == 3) {
                if (tx->data[0] > 253 && tx->data[1] == 1 && tx->data[3] == 11) lc_write_ptt(lc, 1);
                struct bredr_dut *dut = g_dut_hdl;
                if (dut != 0) {
                    if (dut->scenario == 4) BB_REG_B = BB_REG_B | 1;
                    if (dut->start != 0 && lc->test_con_flag == 0) {
                        frame->linkcntl0 = frame->linkcntl0 | 32;
                        frame->linkcntl1 = frame->linkcntl1 | 32;
                        putchar(68);
                    }
                }
            }
            u8 enc_post = tx->enc_post;
            if ((enc_post & 4) != 0) lc_write_encry(lc, (enc_post >> 1) & 1, enc_post & 1);
            u8 event_post = tx->event_post;
            if (event_post != 0) bredr_link_event(lc, 1, event_post);
            if (tx->pwr_post) lc->power.normal.pwr_txing = 0;
            bredr_tx_bulk_free(tx);
        }
        dut_tx_mode(lc);
        struct bredr_tx_bulk *next = bredr_tx_bulk_pop(&lc->tx_acl_c);
        if (next != 0) {
            BB_REG_B = BB_REG_B & ~1u;
            if (lc->test_con_flag == 0) {
                frame->linkcntl0 = frame->linkcntl0 & ~32;
                frame->linkcntl1 = frame->linkcntl1 & ~32;
                if (slot == 0) frame->txbuf1cntl = frame->txbuf1cntl | 1;
                else frame->txbuf0cntl = frame->txbuf0cntl | 1;
            }
        } else {
            next = bredr_tx_bulk_pop(&lc->tx_acl_u);
        }
        if (next != 0) dut_link_ctrl_tx(lc, next, slot);
        lc->tx_bulk[slot] = next;
    }
}

DUT_CODE void dut_rx_handler(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    u8 rxbuf0cntl = frame->rxbuf0cntl;
    u8 rxbuf1cntl = frame->rxbuf1cntl;
    int rx_full = 1;        /* carried across iterations, as in the SDK */
    for (;;) {
        int index;
        if ((frame->rxtog & 1) != 0) {
            if ((rxbuf1cntl & 1) != 0) index = 1;
            else if ((rxbuf0cntl & 1) != 0) index = 0;
            else return;
        } else {
            if ((rxbuf0cntl & 1) != 0) index = 0;
            else if ((rxbuf1cntl & 1) != 0) index = 1;
            else return;
        }
        __write_reg_rxint(frame, index, 0);
        u16 rxstat = (&frame->rxstat0)[index];
        u16 rxphdr = (&frame->rxphdr0)[index];
        u8 llid = (u8)rxphdr & 3;
        u16 flags = rxstat >> 8;
        u16 type = rxstat & 15;
        u16 length = (rxphdr >> 3) & 1023;
        if (lc->adjust_clkoffset_enable && (flags & 2) != 0 && lc->role) __rx_adjust_clkoffset(lc);
        if (flags == 143 || (g_dut_hdl->start != 0 && (flags & 2) != 0)) {
            if (type >= 3 && length != 0) {
                if (llid == 3) {
                    if (type == 3 && flags == 143) {
                        struct bredr_rx_bulk *bulk = bredr_rx_bulk_alloc(0, length);
                        if (bulk != 0) {
                            bulk->len = length;
                            bulk->llid = 3;
                            memcpy(bulk->data, lc->rx_data[index], length);
                        }
                        __clean_reg_rxfull(lc->frame, index);
                        if (list_empty(&lc->rx_head)) bredr_link_event(lc, 1, LMP_EVENT_RX);
                        bredr_rx_bulk_push(&lc->rx_head, bulk);
                        rx_full = 0;
                    }
                } else {
                    struct bredr_rx_bulk *bulk = bredr_rx_bulk_alloc(0, length);
                    if (bulk != 0) {
                        bulk->len = length;
                        bulk->llid = llid;
                        memcpy(bulk->data, lc->rx_data[index], length);
                    }
                    dut_loop_back(lc, bulk, (u8)flags, length);
                    bredr_rx_bulk_free(bulk);
                }
            }
            link_conn_super_timeout_reset(lc);
        } else if ((flags & 18) == 2) {
            link_conn_super_timeout_reset(lc);
        }
        if (rx_full != 0) __clean_reg_rxfull(frame, index);
        dut_tx_handler(lc);
        if (index == 1) {
            rxbuf1cntl = 0;
            if (rxbuf0cntl == 0) return;
        } else {
            rxbuf0cntl = 0;
            if (rxbuf1cntl == 0) return;
        }
    }
}

#ifdef UNITTEST
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "test.h"

volatile u32 dut_reg_c0000, dut_reg_c00d0;
struct bredr_dut *g_dut_hdl;
struct common_rf_param rf;
int config_btctler_mode, CONFIG_TEST_DUT_ONLY_BOX_CODE;
u8 log_tag_const_i_BL;

static char trace[8192];
static void T(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    size_t n = strlen(trace);
    vsnprintf(trace + n, sizeof(trace) - n, format, ap);
    va_end(ap);
}
#define EXPECT(s) do { if (strcmp(trace, (s))) { fprintf(stderr, "trace:    %s\nexpected: %s\n", trace, (s)); CHECK(0); } trace[0] = 0; } while (0)

static struct link_connection lc;
static struct ctrl_frame frame;
static struct bredr_dut dut;
static struct bredr_tx_bulk *pool[16];
static int pooled;
static u8 prng_bytes[16];

void log_print(int level, const char *file, const char *format, ...) { (void)level; (void)file; (void)format; T("log;"); }
int putchar(int c) { T("putchar(%d);", c); return c; }
void local_irq_disable(void) { T("irq_off;"); }
void local_irq_enable(void) { T("irq_on;"); }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size)
{
    struct bredr_tx_bulk *b = calloc(1, sizeof(*b) + size + 8);
    pool[pooled++ % 16] = b;
    T("tx_alloc(%d)=#%d;", size, pooled - 1);
    return b;
}
static int id_of(struct bredr_tx_bulk *b)
{
    for (int i = 0; i < pooled && i < 16; i++) if (pool[i] == b) return i;
    return -1;
}
void bredr_tx_bulk_free(struct bredr_tx_bulk *b) { T("tx_free(#%d);", id_of(b)); }
struct bredr_tx_bulk *bredr_tx_bulk_pop(struct list_head *head)
{
    if (head->next == head) { T("pop(%s)=0;", head == &lc.tx_acl_u ? "u" : "c"); return 0; }
    struct list_head *e = head->next;
    e->prev->next = e->next; e->next->prev = e->prev; e->next = e->prev = e;
    struct bredr_tx_bulk *b = (struct bredr_tx_bulk *)((u8 *)e - offsetof(struct bredr_tx_bulk, entry));
    T("pop(%s)=#%d;", head == &lc.tx_acl_u ? "u" : "c", id_of(b));
    return b;
}
void __write_reg_packet_type(struct ctrl_frame *f, int index, int packet) { CHECK(f == &frame); T("ptype(%d,%d);", index, packet); }
void __write_reg_rxint(struct ctrl_frame *f, int index, u8 enable) { CHECK(f == &frame); T("rxint(%d,%d);", index, enable); }
void __write_reg_txinfo(struct ctrl_frame *f, int index, void *data, int len, int llid, int flow, int packet)
{ (void)data; CHECK(f == &frame); T("txinfo(%d,len=%d,llid=%d,flow=%d,pkt=%d);", index, len, llid, flow, packet); }
void __clean_reg_rxfull(struct ctrl_frame *f, int index) { CHECK(f == &frame); T("rxfull_clean(%d);", index); }
void __clean_reg_txempty(struct ctrl_frame *f, int index) { CHECK(f == &frame); T("txempty_clean(%d);", index); }
void __rx_adjust_clkoffset(struct link_connection *l) { CHECK(l == &lc); T("adjust;"); }
int bredr_link_event(void *link, int argc, ...)
{
    va_list ap; va_start(ap, argc);
    CHECK(link == &lc && argc == 1);
    T("event(%d);", va_arg(ap, int));
    va_end(ap); return 0;
}
static struct bredr_rx_bulk *last_rx;
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int id, int len)
{
    CHECK(id == 0);
    last_rx = calloc(1, sizeof(*last_rx) + len + 8);
    T("rx_alloc(%d);", len);
    return last_rx;
}
void bredr_rx_bulk_push(struct list_head *head, struct bredr_rx_bulk *bulk) { CHECK(head == &lc.rx_head); T("rx_push(len=%d,llid=%d);", bulk->len, bulk->llid); }
void bredr_rx_bulk_free(struct bredr_rx_bulk *bulk) { (void)bulk; T("rx_free;"); }
void link_conn_super_timeout_reset(struct link_connection *l) { CHECK(l == &lc); T("super;"); }
void lc_write_ptt(struct link_connection *l, u8 en) { CHECK(l == &lc); T("ptt(%d);", en); }
void lc_write_encry(struct link_connection *l, u8 rx_enc, u8 tx_enc) { CHECK(l == &lc); T("encry(%d,%d);", rx_enc, tx_enc); }
int hci_send_event(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    CHECK(!strcmp(format, "1")); T("hci_event(%d,%d);", opcode, va_arg(ap, int));
    va_end(ap); return 0;
}
void lmp_free(void *buf) { CHECK(buf == &dut); T("lmp_free;"); }
void pseudo_random_genrate(uint8_t *dest, unsigned int size) { CHECK(size == 16); memcpy(dest, prng_bytes, 16); T("prng;"); }

static void setup(void)
{
    memset(&lc, 0, sizeof(lc)); memset((void *)&frame, 0, sizeof(frame)); memset(&dut, 0, sizeof(dut));
    lc.frame = &frame; g_dut_hdl = &dut;
    lc.tx_acl_u.next = lc.tx_acl_u.prev = &lc.tx_acl_u;
    lc.tx_acl_c.next = lc.tx_acl_c.prev = &lc.tx_acl_c;
    lc.rx_head.next = lc.rx_head.prev = &lc.rx_head;
    dut_reg_c0000 = dut_reg_c00d0 = 0; trace[0] = 0; pooled = 0;
    config_btctler_mode = 0; rf.production_test = 0; memset(prng_bytes, 0xff, 16);
}

static void keep_setup(void) { int p = pooled; setup(); pooled = p; }

static void check_table(const u16 *row, int length, int type) { CHECK(row[0] == length && row[1] == type); }

int main(void)
{
    /* Packet length table lookup: first row with the packet type, else the last row. */
    const u16 *r;
    r = dut_get_packet_max_len(8, 0); check_table(r, 10, 8);
    r = dut_get_packet_max_len(3, 0); check_table(r, 17, 3);
    r = dut_get_packet_max_len(15, 0); check_table(r, 339, 15);
    r = dut_get_packet_max_len(99, 0); check_table(r, 339, 15);
    r = dut_get_packet_max_len(4, 2); check_table(r, 54, 4);
    r = dut_get_packet_max_len(11, 2); check_table(r, 552, 11);
    r = dut_get_packet_max_len(99, 2); check_table(r, 1021, 15);
    r = dut_get_packet_max_len(7, 3); check_table(r, 360, 7);
    r = dut_get_packet_max_len(99, 3); check_table(r, 540, 13);
    r = dut_get_packet_max_len(12, 1); check_table(r, 120, 12);
    r = dut_get_packet_max_len(99, 1); check_table(r, 180, 13);
    r = dut_get_packet_max_len(7, 9); check_table(r, 30, 7);

    /* The PRBS9 byte table is one period of x^9 + x^5 + 1 from all ones, least
     * significant bit first: out[n] = out[n-5] ^ out[n-9]. */
    {
        static u8 bits[511 * 8];
        for (int n = 0; n < 511 * 8; n++) bits[n] = n < 9 ? 1 : bits[n - 5] ^ bits[n - 9];
        for (int j = 0; j < 511; j++) {
            int v = 0;
            for (int b = 0; b < 8; b++) v |= bits[j * 8 + b] << b;
            CHECK(prbs9_table1[j] == v);
        }
        /* position table: for each byte value the positions where it occurs */
        for (int v = 1; v < 256; v++) {
            int found = 0;
            for (int k = 0; k < 2; k++) found += prbs9_table1[prbs9_table0[2 * v - 1 + k]] == v;
            CHECK(found >= 1);
        }
        CHECK(prbs9_table1[0] == 0xff && prbs9_table1[1] == 0xc1 && prbs9_table1[2] == 0xfb && prbs9_table1[3] == 0xe8);
    }

    /* dut_cfg_analog per scenario: (registers A=0x1c0000, B=0x1c00d0). */
    static const struct { u8 scenario; u32 a_in, b_in, a_out, b_out; int start; } sc[] = {
        { 1, 0, 0x0f, 0x80000, 0x0e, 1 },
        { 2, 0, 0x0f, 0x80000, 0x0e, 1 },
        { 3, 0, 0x0f, 0x80000, 0x0e, 1 },
        { 9, 0, 0x0f, 0x80000, 0x0e, 1 },
        { 4, 0, 0x0f, 0x80000, 0x0d, 1 },       /* B: &~1, |=1, &~2 */
        { 5, 0x80000, 0x0f, 0, 0x0e, 1 },
        { 6, 0x80000, 0x0f, 0, 0x0e, 1 },
        { 7, 0, 0x0f, 0x80000, 0x0e, 1 },
        { 8, 0, 0x0f, 0x80000, 0x0e, 1 },
        { 255, 0x80000, 0x0f, 0, 0x0e, 0 },
        { 10, 0x80000, 0x0f, 0x80000, 0x0e, 1 },    /* unknown: start untouched */
    };
    for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
        setup();
        dut.scenario = sc[i].scenario; dut.hopping_mode = 1; dut.start = sc[i].scenario == 10 ? 1 : 7;
        dut_reg_c0000 = sc[i].a_in; dut_reg_c00d0 = sc[i].b_in;
        dut_cfg_analog(&lc);
        CHECK(dut_reg_c0000 == sc[i].a_out);          /* hopping_mode 1 clears 0x10000: not set here */
        CHECK(dut_reg_c00d0 == sc[i].b_out);
        CHECK(dut.start == (sc[i].scenario == 10 ? 1 : sc[i].start));
    }
    /* Scenario 0 stops right after clearing start. */
    setup(); dut.scenario = 0; dut.start = 1; dut.packet_type = 0x35; dut.right_packet_cnt = 9; dut_reg_c00d0 = 0xff;
    dut_cfg_analog(&lc);
    CHECK(dut.start == 0 && dut_reg_c00d0 == 0xfe && dut.packet_len == 0 && dut.right_packet_cnt == 9 && !frame.miscntl);
    /* Fixed hopping: 0x10000 set, format bit 6 cleared, frequencies from the halved index
     * (odd +40); packet type 0x35 = group 3, number 5 (above 31: miscntl bit 3 set). */
    setup(); dut.scenario = 1; dut.hopping_mode = 0; dut.rx_fre = 10; dut.tx_fre = 11; dut.packet_type = 0x35;
    dut.right_packet_cnt = 77; frame.format = 0xff; frame.miscntl = 0x01;
    dut_cfg_analog(&lc);
    CHECK(dut_reg_c0000 == (0x80000 | 0x10000) && frame.format == 0xbf && frame.rxfreq == 5 && frame.txfreq == 45);
    CHECK(dut.right_packet_cnt == 0 && dut.packet_group == 3 && dut.packet_num == 5 && frame.miscntl == 0x09);
    CHECK(dut.packet_len == 540);                      /* row for edr esco type 5: none, last row */
    /* Hopping: format bit 6 set when miscntl > 63; 0x10000 cleared; packet type 8 = group 0. */
    setup(); dut.scenario = 1; dut.hopping_mode = 1; dut.packet_type = 0x08; frame.miscntl = 64 | 8; dut_reg_c0000 = 0x10000;
    dut_cfg_analog(&lc);
    CHECK(dut_reg_c0000 == 0x80000 && frame.format == 64 && frame.miscntl == 64 && dut.packet_len == 10 && dut.packet_num == 8);
    setup(); dut.scenario = 1; dut.hopping_mode = 1; frame.miscntl = 63;
    dut_cfg_analog(&lc);
    CHECK(frame.format == 0);

    /* Detach: scenario becomes 255, the RF is switched off, the handle forgotten. */
    setup(); dut.scenario = 5; dut_reg_c0000 = 0x80000 | 0x10000 | 4; dut_reg_c00d0 = 3;
    bredr_dut_detch(&lc);
    CHECK(g_dut_hdl == 0 && dut.scenario == 255 && dut.start == 0);
    CHECK(dut_reg_c0000 == 4 && dut_reg_c00d0 == 2);
    setup(); lc.frame = 0; bredr_dut_detch(&lc); CHECK(g_dut_hdl == &dut && dut.scenario == 0);
    setup(); bredr_dut_detch(0); CHECK(g_dut_hdl == &dut);
    setup(); g_dut_hdl = 0; bredr_dut_detch(&lc); CHECK(dut_reg_c00d0 == 0);

    /* dut_clear_tx_buf. */
    {
        struct bredr_tx_bulk *b0, *b1, *u0, *u1;
        setup();
        b0 = bredr_tx_bulk_alloc(1); b1 = bredr_tx_bulk_alloc(1); u0 = bredr_tx_bulk_alloc(1); u1 = bredr_tx_bulk_alloc(1);
        trace[0] = 0;
        list_add_tail(&u0->entry, &lc.tx_acl_u); list_add_tail(&u1->entry, &lc.tx_acl_u);
        lc.tx_bulk[0] = b0; lc.tx_bulk[1] = b1;
        dut_clear_tx_buf(&lc);
        EXPECT("irq_off;tx_free(#0);tx_free(#1);tx_free(#2);tx_free(#3);ptype(0,0);ptype(1,0);irq_on;");
        CHECK(!lc.tx_bulk[0] && !lc.tx_bulk[1]);
        /* The same bulk in both slots is freed once. */
        keep_setup(); lc.tx_bulk[0] = lc.tx_bulk[1] = b0;
        dut_clear_tx_buf(&lc);
        EXPECT("irq_off;tx_free(#0);ptype(0,0);ptype(1,0);irq_on;");
        /* Only slot 1 holds a bulk. */
        keep_setup(); lc.tx_bulk[1] = b1; dut_clear_tx_buf(&lc);
        EXPECT("irq_off;tx_free(#1);ptype(0,0);ptype(1,0);irq_on;");
        /* No handle or a test connection: nothing at all. */
        keep_setup(); g_dut_hdl = 0; lc.tx_bulk[0] = b0; dut_clear_tx_buf(&lc); EXPECT("");
        keep_setup(); lc.test_con_flag = 1; lc.tx_bulk[0] = b0; dut_clear_tx_buf(&lc); EXPECT(""); CHECK(lc.tx_bulk[0] == b0);
    }

    /* dut_rx_handler: nothing received. */
    setup(); dut_rx_handler(&lc); EXPECT("");

    /* An LMP packet (llid 3, type 3, flags 0x8f) goes to the link's receive queue. */
    setup();
    static u8 rxbuf[2][64];
    lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    for (int i = 0; i < 64; i++) rxbuf[0][i] = rxbuf[1][i] = i + 1;
    frame.rxbuf0cntl = 1; frame.rxtog = 0;
    frame.rxstat0 = (143 << 8) | 3; frame.rxphdr0 = (5 << 3) | 3;
    dut_rx_handler(&lc);
    EXPECT("rxint(0,0);rx_alloc(5);rxfull_clean(0);event(5);rx_push(len=5,llid=3);super;"
           "pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;");
    CHECK(!memcmp(last_rx->data, rxbuf[0], 5));
    /* The same with the receive queue already holding data: no event.  rxtog 1 takes slot 1 first. */
    {
        struct list_head queued;
        setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
        list_add_tail(&queued, &lc.rx_head);
        frame.rxbuf0cntl = 1; frame.rxbuf1cntl = 1; frame.rxtog = 1;
        frame.rxstat1 = (143 << 8) | 3; frame.rxphdr1 = (4 << 3) | 3;
        frame.rxstat0 = 0; frame.rxphdr0 = 0;
        dut_rx_handler(&lc);
        /* Slot 1 is an LMP packet and sets rx_full to 0.  The loop then runs for slot 0 (its control byte is
         * not zero): no flags, so it is not processed and, rx_full still being 0 from the first pass (the
         * SDK carries it over), its receive buffer is not cleaned. */
        EXPECT("rxint(1,0);rx_alloc(4);rxfull_clean(1);rx_push(len=4,llid=3);super;pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;"
               "rxint(0,0);pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;");
    }

    /* Loop back (scenario 5): a data packet is echoed on the transmit queue. */
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    dut.start = 1; dut.scenario = 5; dut.packet_len = 100; dut.packet_num = 3;
    frame.rxbuf0cntl = 1; frame.rxtog = 0;
    frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (20 << 3) | 2;
    dut_rx_handler(&lc);
    EXPECT("rxint(0,0);rx_alloc(20);tx_alloc(20)=#0;rx_free;super;rxfull_clean(0);"
           "pop(c)=0;pop(u)=#0;txinfo(0,len=20,llid=2,flow=1,pkt=3);txempty_clean(0);"
           "pop(c)=0;pop(u)=0;");
    CHECK(lc.tx_bulk[0] == pool[0] && !lc.tx_bulk[1] && pool[0]->offset == 20 && pool[0]->len == 20);
    CHECK(!memcmp(pool[0]->data, rxbuf[0], 20) && pool[0]->llid == 2 && pool[0]->packet == 3);
    /* A packet longer than packet_len is cut to it. */
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    dut.start = 1; dut.scenario = 8; dut.packet_len = 7; dut.packet_num = 1;
    frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (20 << 3) | 1;
    dut_rx_handler(&lc);
    CHECK(strstr(trace, "tx_alloc(7)=#0;") && strstr(trace, "txinfo(0,len=7,llid=2"));
    /* Scenario 0 or 255, or start 0 with an ordinary packet: no loop back (flags 2 needs start). */
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    dut.start = 1; dut.scenario = 255; frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (20 << 3) | 2;
    dut_rx_handler(&lc);
    EXPECT("rxint(0,0);rx_alloc(20);rx_free;super;rxfull_clean(0);pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;");
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    dut.start = 1; dut.scenario = 3; frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (20 << 3) | 2;
    dut_rx_handler(&lc);              /* scenario 3 is transmit-only: (3 - 5) wraps, no loop back */
    CHECK(!strstr(trace, "tx_alloc(20)"));
    /* start 0 and flags 2: ignored apart from the supervision reset. */
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (20 << 3) | 2;
    dut_rx_handler(&lc);
    EXPECT("rxint(0,0);super;rxfull_clean(0);pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;");
    /* Flags with bit 1 set and bit 4 clear only reset supervision; other flags do nothing. */
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    frame.rxbuf0cntl = 1; frame.rxstat0 = (0x12 << 8) | 4; frame.rxphdr0 = (20 << 3) | 2;
    dut_rx_handler(&lc);
    EXPECT("rxint(0,0);rxfull_clean(0);pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;");
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    frame.rxbuf0cntl = 1; frame.rxstat0 = (0x02 << 8) | 4; frame.rxphdr0 = 0;
    dut_rx_handler(&lc);
    EXPECT("rxint(0,0);super;rxfull_clean(0);pop(c)=0;pop(u)=0;pop(c)=0;pop(u)=0;");
    /* Clock-offset adjustment needs the enable bit, flags bit 1 and the role bit. */
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    lc.adjust_clkoffset_enable = 1; lc.role = 1;
    frame.rxbuf0cntl = 1; frame.rxstat0 = (0x02 << 8); frame.rxphdr0 = 0;
    dut_rx_handler(&lc);
    CHECK(!strncmp(trace, "rxint(0,0);adjust;", 18));
    setup(); lc.rx_data[0] = rxbuf[0]; lc.rx_data[1] = rxbuf[1];
    lc.adjust_clkoffset_enable = 1; frame.rxbuf0cntl = 1; frame.rxstat0 = (0x02 << 8);
    dut_rx_handler(&lc);
    CHECK(!strstr(trace, "adjust"));

    /* Loop back with PRBS9 regeneration (config mode bit 1): the echo continues the PRBS stream. */
    {
        static u8 payload[700];
        for (int i = 0; i < 700; i++) payload[i] = prbs9_table1[i % 511];
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 600; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (600 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(strstr(trace, "tx_alloc(600)=#0;") && strstr(trace, "prng;"));
        for (int i = 0; i < 600; i++) if (pool[0]->data[i] != prbs9_table1[i % 511]) { fprintf(stderr, "prbs byte %d\n", i); CHECK(0); break; }
        CHECK(dut.right_packet_cnt == 1);
        /* A PRBS stream starting mid-sequence: the echo is aligned to it (rx[0] = table position 100). */
        for (int i = 0; i < 700; i++) payload[i] = prbs9_table1[(100 + i) % 511];
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 600; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (600 << 3) | 2;
        dut_rx_handler(&lc);
        for (int i = 0; i < 600; i++) if (pool[0]->data[i] != prbs9_table1[(100 + i) % 511]) { fprintf(stderr, "prbs byte %d\n", i); CHECK(0); break; }
        /* Flags bit 2 set: plain copy instead. */
        for (int i = 0; i < 700; i++) payload[i] = prbs9_table1[(100 + i) % 511] ^ (i > 100 ? 0x5a : 0);
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 600; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (6 << 8) | 4; frame.rxphdr0 = (600 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(!memcmp(pool[0]->data, payload, 600) && !strstr(trace, "prng"));
        /* The random error injection: 200th packet with prng byte 0 & 63 == 0 flips a byte. */
        for (int i = 0; i < 700; i++) payload[i] = prbs9_table1[i % 511];
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 600; dut.packet_num = 2; dut.right_packet_cnt = 199;
        prng_bytes[0] = 0x40; prng_bytes[1] = 13;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (600 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(dut.right_packet_cnt == 0);
        for (int i = 0; i < 600; i++) {
            u8 want = prbs9_table1[i % 511] ^ (i == 13 ? (21 & 13) : 0);      /* length 600 > 299: mask 21, pick 13 */
            if (pool[0]->data[i] != (u8)want) { fprintf(stderr, "err byte %d\n", i); CHECK(0); break; }
        }
        /* Not on that packet: counter 5, prng byte 1. */
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 100; dut.packet_num = 2; dut.right_packet_cnt = 5;
        prng_bytes[0] = 0x41;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (100 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(dut.right_packet_cnt == 6 && !memcmp(pool[0]->data, payload, 100));
        /* Short packet (length 100): mask 1 when the packet is not longer than 299. */
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 100; dut.packet_num = 2; dut.right_packet_cnt = 250;
        prng_bytes[0] = 0x80; prng_bytes[1] = 0xff;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (100 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(pool[0]->data[255 % 100] == (prbs9_table1[55] ^ 1));
        /* production_test with both frequencies set: plain copy; with one zero: the checker. */
        for (int i = 0; i < 700; i++) payload[i] = prbs9_table1[(100 + i) % 511] ^ (i > 100 ? 0x5a : 0);
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        rf.production_test = 1; dut.rx_fre = 1; dut.tx_fre = 1;
        dut.start = 1; dut.scenario = 5; dut.packet_len = 200; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (200 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(!memcmp(pool[0]->data, payload, 200) && !strstr(trace, "prng"));
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        rf.production_test = 1; dut.rx_fre = 1; dut.tx_fre = 0;
        dut.start = 1; dut.scenario = 5; dut.packet_len = 200; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (200 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(strstr(trace, "prng;"));
        /* Repeating payload (0x55 0xaa pairs in the first 48 bytes): tiled in blocks of 48. */
        for (int i = 0; i < 48; i++) payload[i] = i % 2 ? 0xaa : 0x55;
        for (int i = 48; i < 200; i++) payload[i] = 0x11;
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 130; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (130 << 3) | 2;
        dut_rx_handler(&lc);
        for (int i = 0; i < 130; i++) if (pool[0]->data[i] != payload[i % 48]) { fprintf(stderr, "tile byte %d\n", i); CHECK(0); break; }
        /* A repeating payload starting with another value is copied. */
        for (int i = 0; i < 48; i++) payload[i] = i % 2 ? 0x55 : 0xaa;
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 130; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (130 << 3) | 2;
        dut_rx_handler(&lc);
        CHECK(!memcmp(pool[0]->data, payload, 130));
        /* All zeros also tile (value 0). */
        memset(payload, 0, 48); for (int i = 48; i < 200; i++) payload[i] = 0x22;
        setup(); lc.rx_data[0] = payload; lc.rx_data[1] = rxbuf[1];
        config_btctler_mode = 2; dut.start = 1; dut.scenario = 5; dut.packet_len = 100; dut.packet_num = 2;
        frame.rxbuf0cntl = 1; frame.rxstat0 = (2 << 8) | 4; frame.rxphdr0 = (100 << 3) | 2;
        dut_rx_handler(&lc);
        for (int i = 0; i < 100; i++) if (pool[0]->data[i] != 0) { fprintf(stderr, "zero tile byte %d\n", i); CHECK(0); break; }
    }

    /* Transmit side.  A finished control bulk in slot 0 with the DUT running. */
    {
        struct bredr_tx_bulk *done, *ctrl;
        setup(); dut.start = 1; dut.scenario = 4; dut.packet_len = 12; dut.packet_num = 6;
        done = bredr_tx_bulk_alloc(4); ctrl = bredr_tx_bulk_alloc(3); trace[0] = 0;
        done->llid = 3; done->data[0] = 0xfe; done->data[1] = 1; done->data[3] = 11;
        done->len = done->offset = 4; done->enc_post = 7; done->event_post = 9; done->pwr_post = 1;
        lc.power.normal.pwr_txing = 1; lc.power.normal.pwr_tx_cnt = 5;
        lc.tx_bulk[0] = done;
        frame.txbuf0cntl = 1; frame.txtog = 0;
        ctrl->llid = 3; ctrl->len = 3; ctrl->enc_prob = 5; ctrl->event_prob = 7; ctrl->packet = 2;
        list_add_tail(&ctrl->entry, &lc.tx_acl_c);
        frame.linkcntl0 = 0x30; frame.linkcntl1 = 0x30;
        dut_reg_c00d0 = 0x0e;
        dut_rx_handler(&lc);                 /* no receive slot ready: the handler returns at once */
        EXPECT("");
        frame.rxbuf0cntl = 1; frame.rxstat0 = 0; frame.rxphdr0 = 0;
        dut_rx_handler(&lc);
        /* rx slot 0 cleaned (no flags), then the transmit slots: slot 0 holds the finished bulk:
         * ptt, encryption, event, pwr flag cleared, free; DUT scenario 4 queues a 12-byte test packet; the
         * control queue's bulk goes out first.  Register B bit 0 is set by the finished bulk (scenario 4)
         * and cleared again when the control bulk is taken. */
        EXPECT("rxint(0,0);rxfull_clean(0);ptt(1);putchar(68);encry(1,1);event(9);tx_free(#0);tx_alloc(12)=#2;pop(c)=#1;"
               "encry(0,1);event(7);txinfo(0,len=3,llid=3,flow=1,pkt=2);txempty_clean(0);"
               "tx_alloc(12)=#3;pop(c)=0;pop(u)=#2;txinfo(1,len=12,llid=2,flow=1,pkt=6);txempty_clean(1);");
        CHECK(lc.tx_bulk[0] == ctrl && lc.tx_bulk[1] == pool[2]);
        CHECK(lc.power.normal.pwr_txing == 0 && lc.power.normal.pwr_tx_cnt == 5);
        CHECK(dut_reg_c00d0 == 0x0e && frame.txbuf1cntl == 1 && frame.txbuf0cntl == 1);
        CHECK((frame.linkcntl0 & 32) == 0 && (frame.linkcntl1 & 32) == 0);
    }
    return TEST_DONE("btctrler_bredr_dut");
}
#endif
