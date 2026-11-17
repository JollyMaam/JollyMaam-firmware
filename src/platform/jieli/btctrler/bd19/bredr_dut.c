/* BD19 occurrence 1 of bredr_dut.c.o. The public entry points preserve the
 * member's callback order, two-slot frame handshake and target MMIO sequence.
 * Retained SDK quirks include null RX bulk enqueue/copy paths, an unchecked
 * g_dut_hdl dereference in the TX refill path, overwriting an unfinished TX
 * slot, and detach clearing the global handle without freeing its object. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

int putchar(int c);
#ifdef UNITTEST
extern int CONFIG_TEST_DUT_ONLY_BOX_CODE;
extern u32 dut_reg_1c0000, dut_reg_1c0100, dut_reg_1c011c;
#else
extern const int CONFIG_TEST_DUT_ONLY_BOX_CODE;
#endif

#define DUT_CODE __attribute__((section(".classic_rf_code")))
#define DUT_CONST __attribute__((section(".classic_rf_const")))

#ifdef UNITTEST
extern u32 dut_read32(u32 address);
extern u32 dut_poll32(u32 address);
extern void dut_write32(u32 address, u32 value);
#define DUT_READ32(address) dut_read32(address)
#define DUT_POLL32(address) dut_poll32(address)
#define DUT_WRITE32(address, value) dut_write32((address), (value))
#else
#define DUT_READ32(address) (*(volatile u32 *)(uintptr_t)(address))
#define DUT_POLL32(address) DUT_READ32(address)
#define DUT_WRITE32(address, value) (*(volatile u32 *)(uintptr_t)(address) = (value))
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

static inline __attribute__((always_inline)) u8 cal_hop_fre(u8 fre)
{
    int half = fre >> 1;
    return (fre & 1) == 0 ? half : half + 40;
}

/* Configure the baseband for the scenario in g_dut_hdl. */
DUT_CODE void dut_cfg_analog(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    struct bredr_dut *dut = g_dut_hdl;
    DUT_WRITE32(0x1c0100u, DUT_READ32(0x1c0100u) & ~8u);
    switch (dut->scenario) {
    case 0:
        dut->start = 0;
        return;
    case 4:
        DUT_WRITE32(0x1c0100u, DUT_READ32(0x1c0100u) | 8u);
        DUT_WRITE32(0x1c0100u, DUT_READ32(0x1c0100u) & ~4u);
        /* fall through */
    case 1: case 2: case 3: case 9:
        frame->optcntl |= 1u;
        dut->start = 1;
        break;
    case 5: case 6:
        frame->optcntl &= ~1u;
        dut->start = 1;
        break;
    case 7: case 8:
        frame->optcntl |= 1u;
        dut->start = 1;
        break;
    case 255:
        frame->optcntl &= ~1u;
        dut->start = 0;
        break;
    default:
        break;
    }
    if (dut->hopping_mode != 0) {
        frame->format &= ~128u;
    } else {
        frame->format |= 128u;
        DUT_WRITE32(0x1c011cu, 0);
        u8 tx_freq = cal_hop_fre(dut->tx_fre);
        u32 reg = DUT_READ32(0x1c011cu);
        DUT_WRITE32(0x1c011cu, reg | tx_freq);
        u8 rx_freq = cal_hop_fre(dut->rx_fre);
        reg = DUT_READ32(0x1c011cu);
        DUT_WRITE32(0x1c011cu, reg | ((u32)rx_freq << 7));
    }
    dut->right_packet_cnt = 0;
    u8 packet_type = dut->packet_type;
    dut->packet_group = packet_type >> 4;
    dut->packet_num = packet_type & 15;
    u16 miscntl = frame->miscntl;
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
    struct bredr_tx_bulk *first = lc->tx_bulk[0];
    struct bredr_tx_bulk *second;
    if (first == 0) {
        second = lc->tx_bulk[1];
    } else {
        bredr_tx_bulk_free(first);
        second = lc->tx_bulk[1];
        if (second == lc->tx_bulk[0]) {
            lc->tx_bulk[1] = 0;
            second = 0;
        }
        lc->tx_bulk[0] = 0;
    }
    if (second != 0) bredr_tx_bulk_free(second);
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
        struct ctrl_frame *frame = lc->frame;
        DUT_WRITE32(0x1c0100u, DUT_READ32(0x1c0100u) & ~8u);
        frame->format &= ~128u;
        frame->optcntl &= ~1u;
        g_dut_hdl = 0;
    }
}

/* Fill the transmit bulk from the received one: a PRBS9 pattern is
 * regenerated from the position found in the first bytes when the received
 * payload looks like one (many distinct byte values); a repeating payload is
 * tiled; anything else is copied. */
static inline __attribute__((always_inline)) DUT_CODE void check_rx_fill_tx_data(
    struct bredr_rx_bulk *rx, struct bredr_tx_bulk *tx, u8 check,
    struct bredr_dut *dut)
{
    u8 same_table[50][2];
    u8 tmp_buf[16];
    __builtin_memset(&same_table[0][1], 0, 99);
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
                __builtin_memcpy(tx->data, rx->data, len);
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
                __builtin_memcpy(tx->data, prbs9_table1 + from, first_run);
                remaining = (u16)(len - first_run);
                done = (u16)first_run;
            } else {
                __builtin_memcpy(tx->data, prbs9_table1 + from, len);
                done = 0;
                remaining = 0;
            }
            while (remaining != 0) {
                if (remaining < 512) {
                    __builtin_memcpy(tx->data + done, prbs9_table1, remaining);
                    remaining = 0;
                } else {
                    __builtin_memcpy(tx->data + done, prbs9_table1, 511);
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
        for (i = 0; i < blocks; i++) __builtin_memcpy(tx->data + i * 48, rx->data, 48);
        __builtin_memcpy(tx->data + blocks * 48, rx->data, len % 48);
    } else {
        __builtin_memcpy(tx->data, rx->data, len);
    }
}

/* Loop a received payload back as the next transmit packet. */
static inline __attribute__((always_inline)) DUT_CODE void dut_loop_back(
    struct link_connection *lc, struct bredr_rx_bulk *rx, u8 check, int rx_len)
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
    } else {
        __builtin_memcpy(tx->data, rx->data, len);
    }
    list_add_tail(&tx->entry, &lc->tx_acl_u);
}

/* Move a bulk into transmit slot index of the frame. */
static inline __attribute__((always_inline)) DUT_CODE void dut_link_ctrl_tx(
    struct link_connection *lc, struct bredr_tx_bulk *tx, int index)
{
    struct ctrl_frame *frame = lc->frame;
    u8 enc_prob = tx->enc_prob;
    if ((enc_prob & 4) != 0) link_conn_set_encrypt(lc, (enc_prob >> 1) & 1, enc_prob & 1);
    u8 event_prob = tx->event_prob;
    if (event_prob != 0) bredr_link_event(lc, 1, event_prob);
    __write_reg_txinfo(frame, index, tx->data, tx->len, tx->llid, 1, tx->packet);
    tx->offset = tx->len;
    list_del(&tx->entry);
    __clean_reg_txempty(frame, index);
}

/* The SDK's dut_tx_handler: complete finished transmit slots, then refill
 * them from the control queue or the DUT queue. */
DUT_CODE void dut_frame_get_empty_txbuf_index(struct ctrl_frame *frame, u8 *index)
{
    u16 txtog, txbuf0cntl, txbuf1cntl, linkcntl0, linkcntl1;
    local_irq_disable();
    for (;;) {
        DUT_WRITE32(0x1c0000u, DUT_READ32(0x1c0000u) & ~0x70u);
        DUT_WRITE32(0x1c0000u, ((u32)frame->id << 4) | DUT_READ32(0x1c0000u));
        DUT_WRITE32(0x1c0000u, DUT_READ32(0x1c0000u) | 8u);
        txtog = frame->txtog;
        txbuf0cntl = frame->txbuf0cntl;
        txbuf1cntl = frame->txbuf1cntl;
        linkcntl0 = frame->linkcntl0;
        linkcntl1 = frame->linkcntl1;
        if ((DUT_POLL32(0x1c0000u) & 8u) != 0) break;
    }
    u8 toggle = txtog & 1;
    local_irq_enable();
    for (int i = 0; i < 2; ++i) {
        if (toggle) {
            if ((txbuf1cntl & 1) || (linkcntl1 & 14) == 0) index[i] = toggle;
        } else if ((txbuf0cntl & 1) || (linkcntl0 & 14) == 0) {
            index[i] = 0;
        }
        toggle = !toggle;
    }
}

static inline __attribute__((always_inline)) DUT_CODE void dut_tx_handler(
    struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    u16 update = 0xffff;
    dut_frame_get_empty_txbuf_index(frame, (u8 *)&update);
    for (int i = 0; i < 2; i++) {
        int slot = ((u8 *)&update)[i];
        if (slot == 255) continue;

        struct bredr_tx_bulk *tx = lc->tx_bulk[slot];
        if (tx != 0 && tx != lc->tx_bulk[slot == 0] && tx->offset == tx->len) {
            if (tx->dut_post) {
                dut_cfg_analog(lc);
                if (g_dut_hdl->scenario == 255) {
                    hci_send_event(250, "1", 0);
                    lmp_free(g_dut_hdl);
                    g_dut_hdl = 0;
                    bredr_frame_set_irq_handler(frame, lc,
                                                bredr_frame_conn_rx_irq_handler);
                }
            }
            if (tx->llid == 3) {
                u8 first = tx->data[0];
                if (first > 253 && tx->data[1] == 1 && tx->data[3] == 11)
                    link_conn_set_ptt(lc, 1);
                if ((first & 0xfe) == 6 && tx->data[1] == 57)
                    putchar(89);
                struct bredr_dut *dut = g_dut_hdl;
                if (dut != 0) {
                    if (dut->scenario == 4)
                        DUT_WRITE32(0x1c0100u, DUT_READ32(0x1c0100u) | 8u);
                    if (dut->start != 0 && lc->test_con_flag == 0) {
                        frame->linkcntl0 |= 256;
                        frame->linkcntl1 |= 256;
                    }
                }
            }

            u8 *completion_flags = (u8 *)tx +
                offsetof(struct bredr_tx_bulk, max_slot_post) + 1;
            if ((*completion_flags & 1) != 0)
                lc->power.normal.pwr_txing = 0;
            if ((*completion_flags & 4) != 0)
                lc->unsniff_ack = 1;

            u8 enc_post = tx->enc_post;
            if ((enc_post & 4) != 0)
                link_conn_set_encrypt(lc, (enc_post >> 1) & 1, enc_post & 1);
            u8 max_slot_post = tx->max_slot_post;
            if (max_slot_post != 0) lc->core.max_slot = max_slot_post;
            u8 event_post = tx->event_post;
            if (event_post == 0 || lc->tws_link == 0) {
                if (event_post != 0) bredr_link_event(lc, 1, event_post);
                bredr_tx_bulk_free(tx);
            }
        }

        struct bredr_dut *dut = g_dut_hdl;
        if (dut->start != 0) {
            u8 scenario = dut->scenario;
            if (scenario == 9 || scenario == 4 || scenario == 1 ||
                scenario == 2 || scenario == 3) {
                u16 len = dut->data_len == 0 ? dut->packet_len : dut->data_len;
                struct bredr_tx_bulk *generated = bredr_tx_bulk_alloc(len);
                if (generated != 0) {
                    generated->packet = (u8)dut->packet_num;
                    generated->offset = 0;
                    generated->len = len;
                    generated->event_post = 0;
                    generated->llid = 2;
                    if (dut->scenario != 4) {
                        u8 fill_scenario = dut->scenario;
                        __builtin_memset(generated->data,
                                         value_table[fill_scenario], len);
                    }
                    list_add_tail(&generated->entry, &lc->tx_acl_u);
                }
            }
        }

        struct bredr_tx_bulk *next = bredr_tx_bulk_pop(&lc->tx_acl_c);
        if (next != 0) {
            DUT_WRITE32(0x1c0100u, DUT_READ32(0x1c0100u) & ~8u);
            if (lc->test_con_flag == 0) {
                frame->linkcntl0 &= ~256u;
                frame->linkcntl1 &= ~256u;
                if (slot == 0) frame->txbuf1cntl |= 1;
                else frame->txbuf0cntl |= 1;
            }
        } else {
            next = bredr_tx_bulk_pop(&lc->tx_acl_u);
        }
        if (next != 0) dut_link_ctrl_tx(lc, next, slot);
        lc->tx_bulk[slot] = next;
    }
}

extern void dut_soft_agc(struct link_connection *, u8) __asm__("soft_agc");

DUT_CODE void dut_rx_handler(struct link_connection *lc)
{
    struct bredr_payload_head heads[2];
    struct ctrl_frame *frame = lc->frame;
    bredr_frame_get_rx_payload_head(frame, heads);
    u8 adjusted = 0;

    for (int slot = 0; slot < 2; slot++) {
        u8 index = heads[slot].index;
        if (index == 255) break;
        u8 type = heads[slot].type;
        u8 llid = heads[slot].llid;
        u16 length = heads[slot].len;
        u16 check = heads[slot].check;

        if (adjusted == 0 && (check & 2) != 0) {
            adjusted = 1;
            if (lc->adjust_clkoffset_enable && lc->role)
                rxoffset_cal(frame, 1);
        }
        dut_soft_agc(lc, (u8)check);
        if (check != 127 && (g_dut_hdl->start == 0 || (check & 2) == 0)) {
            __clean_reg_rxfull(frame, index);
            continue;
        }

        if (type == 0 || type == 1 || type == 2 || type == 9 || length == 0) {
            link_conn_super_timeout_reset(lc);
            lc->seqn_err = 0;
            __clean_reg_rxfull(frame, index);
            continue;
        }

        if (llid == 3) {
            if (type == 3 && check == 127) {
                u8 *data = lc->rx_data[index];
                if ((data[0] & 0xfe) == 120) {
                    u32 instant = (u32)data[1] | ((u32)data[2] << 8) |
                                  ((u32)data[3] << 16) | ((u32)data[4] << 24);
                    bredr_link_set_afh(lc, instant, data[5], data + 6);
                } else {
                    struct bredr_rx_bulk *bulk = bredr_rx_bulk_alloc(0, length);
                    if (bulk != 0) {
                        bulk->len = length;
                        bulk->llid = 3;
                        __builtin_memcpy(bulk->data, data, length);
                    }
                    if (list_empty(&lc->rx_head))
                        bredr_link_event(lc, 1, LMP_EVENT_RX);
                    bredr_rx_bulk_push(&lc->rx_head, bulk);
                }
            }
        } else {
            struct bredr_rx_bulk *bulk = bredr_rx_bulk_alloc(0, length);
            if (bulk != 0) {
                bulk->len = length;
                bulk->llid = llid;
                __builtin_memcpy(bulk->data, lc->rx_data[index], length);
            }
            dut_loop_back(lc, bulk, (u8)check, length);
            bredr_rx_bulk_free(bulk);
        }
        link_conn_super_timeout_reset(lc);
        lc->seqn_err = 0;
        __clean_reg_rxfull(frame, index);
    }
    dut_tx_handler(lc);
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

struct bredr_dut *g_dut_hdl;
int CONFIG_TEST_DUT_ONLY_BOX_CODE;
int config_btctler_mode;
u32 dut_reg_1c0000, dut_reg_1c0100, dut_reg_1c011c;

static struct link_connection test_lc, *lc_ptr = &test_lc;
static struct ctrl_frame test_frame, *frame_ptr = &test_frame;
static struct bredr_dut test_dut, test_other_dut;
static struct bredr_payload_head test_heads[2];
static struct bredr_tx_bulk *slot_replacement;
static struct bredr_tx_bulk *tx_allocations[8];
static u8 tx_storage[8][sizeof(struct bredr_tx_bulk) + 1024] __attribute__((aligned(4)));
static unsigned tx_alloc_count, tx_free_count, rx_alloc_count, rx_push_count;
static unsigned rx_free_count, clean_count, timeout_count, soft_agc_count, rxoffset_count;
static unsigned afh_count, event_count, encrypt_count, irq_disable_count, irq_enable_count;
static unsigned irq_active, poll_count, slot_write_count;
static unsigned slot_writes[12];
static unsigned slot_write_values[12];
static int scripted_slot_poll, fail_rx_alloc, mutate_on_tx_free;
static int mutate_scenario_on_tx_alloc;
static u8 scenario_after_tx_alloc;
static struct bredr_tx_bulk *encrypt_mutation_tx;
static int mutate_event_on_encrypt;
static struct bredr_tx_bulk *last_freed_tx;
static struct bredr_rx_bulk *last_pushed_rx;
static struct bredr_dut *hci_replacement, *last_lmp_free;
static struct ctrl_frame *last_irq_frame;
static struct link_connection *last_irq_link;

u32 dut_read32(u32 address)
{
    if (address == 0x1c0000) return dut_reg_1c0000;
    if (address == 0x1c0100) return dut_reg_1c0100;
    if (address == 0x1c011c) return dut_reg_1c011c;
    CHECK(0);
    return 0;
}

u32 dut_poll32(u32 address)
{
    CHECK(address == 0x1c0000 && irq_active == 1);
    poll_count++;
    if (scripted_slot_poll && poll_count == 1) {
        dut_reg_1c0000 &= ~8u;
        test_frame.txtog = 0;
        test_frame.txbuf0cntl = 1;
        test_frame.txbuf1cntl = 1;
        test_frame.linkcntl0 = 2;
        test_frame.linkcntl1 = 0;
    } else if (scripted_slot_poll && poll_count == 2) {
        dut_reg_1c0000 |= 8u;
        test_frame.txtog = 1;
        test_frame.txbuf0cntl = 0;
        test_frame.txbuf1cntl = 0;
        test_frame.linkcntl0 = 2;
        test_frame.linkcntl1 = 2;
    }
    return dut_reg_1c0000;
}

void dut_write32(u32 address, u32 value)
{
    if (address == 0x1c0000) {
        if (slot_write_count < 12) {
            slot_writes[slot_write_count] = address;
            slot_write_values[slot_write_count] = value;
        }
        slot_write_count++;
        dut_reg_1c0000 = value;
    } else if (address == 0x1c0100) {
        dut_reg_1c0100 = value;
    } else if (address == 0x1c011c) {
        dut_reg_1c011c = value;
    } else {
        CHECK(0);
    }
}

void local_irq_disable(void) { irq_active = 1; irq_disable_count++; }
void local_irq_enable(void) { CHECK(irq_active == 1); irq_active = 0; irq_enable_count++; }
void bredr_frame_get_rx_payload_head(struct ctrl_frame *frame, struct bredr_payload_head *out)
{
    CHECK(frame == frame_ptr);
    __builtin_memcpy(out, test_heads, sizeof(test_heads));
}
void rxoffset_cal(struct ctrl_frame *frame, int enable)
{ CHECK(frame == frame_ptr && enable == 1); rxoffset_count++; }
static void (*soft_agc_callback)(struct link_connection *, u8);
void dut_soft_agc(struct link_connection *lc, u8 check) __asm__("soft_agc");
void dut_soft_agc(struct link_connection *lc, u8 check)
{
    CHECK(lc == lc_ptr);
    soft_agc_count++;
    if (soft_agc_callback != 0) soft_agc_callback(lc, check);
}
static void stop_dut_in_agc(struct link_connection *lc, u8 check)
{ CHECK(lc == lc_ptr && check == 2); test_dut.start = 0; }
void bredr_link_set_afh(struct link_connection *lc, u32 instant, u8 enable, u8 *map)
{ CHECK(lc == lc_ptr && map != 0); afh_count++; (void)instant; (void)enable; }
void __clean_reg_rxfull(struct ctrl_frame *frame, int index)
{ CHECK(frame == frame_ptr && (index == 0 || index == 1)); clean_count++; }
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int id, int len)
{
    CHECK(id == 0 && len >= 0);
    rx_alloc_count++;
    if (fail_rx_alloc) return 0;
    return calloc(1, sizeof(struct bredr_rx_bulk) + (unsigned)len + 4);
}
void bredr_rx_bulk_push(struct list_head *head, struct bredr_rx_bulk *bulk)
{ CHECK(head == &test_lc.rx_head); last_pushed_rx = bulk; rx_push_count++; }
void bredr_rx_bulk_free(struct bredr_rx_bulk *bulk)
{ if (bulk != 0) free(bulk); rx_free_count++; }
void link_conn_super_timeout_reset(struct link_connection *lc)
{ CHECK(lc == lc_ptr); timeout_count++; }
int bredr_link_event(void *link, int argc, ...)
{ CHECK(link == lc_ptr && argc == 1); event_count++; return 0; }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int len)
{
    CHECK(len >= 0 && len <= 1024 && tx_alloc_count < 8);
    struct bredr_tx_bulk *tx = (struct bredr_tx_bulk *)tx_storage[tx_alloc_count];
    __builtin_memset(tx, 0, sizeof(tx_storage[0]));
    tx_allocations[tx_alloc_count++] = tx;
    if (mutate_scenario_on_tx_alloc)
        test_dut.scenario = scenario_after_tx_alloc;
    return tx;
}
void bredr_tx_bulk_free(struct bredr_tx_bulk *tx)
{
    tx_free_count++;
    last_freed_tx = tx;
    if (mutate_on_tx_free && tx != slot_replacement) {
        test_lc.tx_bulk[0] = slot_replacement;
        test_lc.tx_bulk[1] = slot_replacement;
    }
}
struct bredr_tx_bulk *bredr_tx_bulk_pop(struct list_head *head)
{
    if (head->next == head) return 0;
    struct list_head *entry = head->next;
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry->prev = entry;
    return (struct bredr_tx_bulk *)((u8 *)entry - offsetof(struct bredr_tx_bulk, entry));
}
void __write_reg_packet_type(struct ctrl_frame *frame, int index, int packet)
{ CHECK(frame == frame_ptr && (index == 0 || index == 1) && packet == 0); }
void __write_reg_txinfo(struct ctrl_frame *frame, int index, void *data,
                        int len, int llid, int flow, int packet)
{ CHECK(frame == frame_ptr && index >= 0 && index <= 1); (void)data; (void)len; (void)llid; (void)flow; (void)packet; }
void __clean_reg_txempty(struct ctrl_frame *frame, int index)
{ CHECK(frame == frame_ptr && (index == 0 || index == 1)); }
void link_conn_set_ptt(struct link_connection *lc, u8 enable)
{ CHECK(lc == lc_ptr && enable == 1); }
static void encrypt_hook(struct link_connection *lc, u8 rx, u8 tx);
void link_conn_set_encrypt(struct link_connection *lc, u8 rx, u8 tx)
{ CHECK(lc == lc_ptr); encrypt_count++; encrypt_hook(lc, rx, tx); }
static void encrypt_hook(struct link_connection *lc, u8 rx, u8 tx)
{
    (void)rx; (void)tx;
    if (mutate_event_on_encrypt) {
        encrypt_mutation_tx->event_post = 9;
        lc->tws_link = 1;
    }
}
int hci_send_event(int opcode, const char *format, ...)
{
    CHECK(opcode == 250 && strcmp(format, "1") == 0);
    g_dut_hdl = hci_replacement;
    return 0;
}
void lmp_free(void *ptr) { last_lmp_free = ptr; }
void bredr_frame_conn_rx_irq_handler(void *link) { (void)link; }
void bredr_frame_set_irq_handler(struct ctrl_frame *frame, void *priv, void (*handler)(void *))
{
    CHECK(frame == frame_ptr && priv == lc_ptr && handler == bredr_frame_conn_rx_irq_handler);
    last_irq_frame = frame;
    last_irq_link = priv;
    g_dut_hdl = &test_dut;
    test_dut.start = 0;
}
int putchar(int c) { return c; }
void pseudo_random_genrate(u8 *dest, u32 size)
{ CHECK(size == 16); __builtin_memset(dest, 0xff, size); }

static void reset_fixture(void)
{
    __builtin_memset(&test_lc, 0, sizeof(test_lc));
    __builtin_memset(&test_frame, 0, sizeof(test_frame));
    __builtin_memset(&test_dut, 0, sizeof(test_dut));
    __builtin_memset(&test_other_dut, 0, sizeof(test_other_dut));
    __builtin_memset(test_heads, 0xff, sizeof(test_heads));
    test_lc.frame = &test_frame;
    frame_ptr = &test_frame;
    lc_ptr = &test_lc;
    test_lc.rx_head.next = test_lc.rx_head.prev = &test_lc.rx_head;
    test_lc.tx_acl_u.next = test_lc.tx_acl_u.prev = &test_lc.tx_acl_u;
    test_lc.tx_acl_c.next = test_lc.tx_acl_c.prev = &test_lc.tx_acl_c;
    g_dut_hdl = &test_dut;
    CONFIG_TEST_DUT_ONLY_BOX_CODE = 0;
    config_btctler_mode = 0;
    dut_reg_1c0000 = dut_reg_1c0100 = dut_reg_1c011c = 0;
    tx_alloc_count = tx_free_count = rx_alloc_count = rx_push_count = 0;
    rx_free_count = clean_count = timeout_count = soft_agc_count = 0;
    afh_count = event_count = encrypt_count = rxoffset_count = 0;
    irq_disable_count = irq_enable_count = irq_active = 0;
    poll_count = slot_write_count = 0;
    scripted_slot_poll = fail_rx_alloc = mutate_on_tx_free = 0;
    mutate_scenario_on_tx_alloc = 0;
    encrypt_mutation_tx = 0;
    mutate_event_on_encrypt = 0;
    soft_agc_callback = 0;
    last_freed_tx = 0; last_pushed_rx = 0; hci_replacement = 0; last_lmp_free = 0;
    last_irq_frame = 0; last_irq_link = 0; slot_replacement = 0;
}

static void test_slot_handshake(void)
{
    u8 updates[2] = { 0xaa, 0xbb };
    reset_fixture();
    test_frame.id = 3;
    test_frame.txtog = 1;
    test_frame.txbuf1cntl = 1;
    test_frame.linkcntl0 = 2;
    test_frame.linkcntl1 = 2;
    scripted_slot_poll = 1;
    dut_frame_get_empty_txbuf_index(&test_frame, updates);
    CHECK(poll_count == 2);
    CHECK(slot_write_count == 6);
    CHECK(slot_write_values[0] == 0 && slot_write_values[1] == 48 && slot_write_values[2] == 56);
    CHECK(slot_write_values[3] == 0 && slot_write_values[4] == 48 && slot_write_values[5] == 56);
    CHECK(updates[0] == 0 && updates[1] == 1);
    CHECK(irq_disable_count == 1 && irq_enable_count == 1 && irq_active == 0);
}

static u32 hash_byte(u32 hash, u8 value)
{ return (hash ^ value) * 16777619u; }

static void test_sdk_tables(void)
{
    static const u16 acl[10][2] = {
        {10,8},{10,5},{17,3},{20,6},{27,4},{30,7},{121,10},{183,11},{224,14},{339,15}
    };
    static const u16 sco[3][2] = {{30,7},{120,12},{180,13}};
    static const u16 edr_acl[6][2] = {
        {54,4},{83,8},{367,10},{552,11},{679,14},{1021,15}
    };
    static const u16 edr_sco[4][2] = {{60,6},{360,7},{90,12},{540,13}};
    const struct { const u16 (*rows)[2]; unsigned count; u8 group; } tables[] = {
        {acl,10,0},{sco,3,1},{edr_acl,6,2},{edr_sco,4,3}
    };
    u32 hash0 = 2166136261u, hash1 = 2166136261u;
    for (unsigned t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        for (unsigned i = 0; i < tables[t].count; i++) {
            const u16 *row = dut_get_packet_max_len(tables[t].rows[i][1], tables[t].group);
            CHECK(row[0] == tables[t].rows[i][0]);
            CHECK(row[1] == tables[t].rows[i][1]);
        }
        const u16 *fallback = dut_get_packet_max_len(99, tables[t].group);
        CHECK(fallback[0] == tables[t].rows[tables[t].count - 1][0]);
        CHECK(fallback[1] == tables[t].rows[tables[t].count - 1][1]);
    }
    for (unsigned i = 0; i < 511; i++) {
        hash0 = hash_byte(hash0, (u8)prbs9_table0[i]);
        hash0 = hash_byte(hash0, (u8)(prbs9_table0[i] >> 8));
        hash1 = hash_byte(hash1, prbs9_table1[i]);
    }
    CHECK(hash0 == 0xc59eaff9u && hash1 == 0x712e547fu);
}

static void test_prbs_and_loopback_fill(void)
{
    u8 rx_storage[offsetof(struct bredr_rx_bulk, data) + 800] __attribute__((aligned(4)));
    u8 sequence[511];
    u8 bits[511 * 8];
    for (unsigned n = 0; n < sizeof(bits); n++)
        bits[n] = n < 9 ? 1 : (u8)(bits[n - 5] ^ bits[n - 9]);
    for (unsigned j = 0; j < sizeof(sequence); j++) {
        sequence[j] = 0;
        for (unsigned bit = 0; bit < 8; bit++)
            sequence[j] |= (u8)(bits[j * 8 + bit] << bit);
    }

    struct bredr_rx_bulk *rx = (struct bredr_rx_bulk *)rx_storage;
    struct bredr_tx_bulk *tx = (struct bredr_tx_bulk *)tx_storage[0];
    __builtin_memset(rx_storage, 0, sizeof(rx_storage));
    __builtin_memset(tx_storage[0], 0, sizeof(tx_storage[0]));
    for (unsigned i = 0; i < 64; i++) rx->data[i] = (u8)i;
    tx->len = 64;
    check_rx_fill_tx_data(rx, tx, 4, &test_dut);
    CHECK(__builtin_memcmp(rx->data, tx->data, 64) == 0);

    __builtin_memset(rx->data, 0x55, 130);
    __builtin_memset(tx_storage[0], 0, sizeof(tx_storage[0]));
    tx->len = 130;
    check_rx_fill_tx_data(rx, tx, 0, &test_dut);
    for (unsigned i = 0; i < 130; i++) CHECK(tx->data[i] == 0x55);

    __builtin_memset(rx->data, 0x33, 64);
    __builtin_memset(tx_storage[0], 0, sizeof(tx_storage[0]));
    tx->len = 64;
    check_rx_fill_tx_data(rx, tx, 0, &test_dut);
    for (unsigned i = 0; i < 64; i++) CHECK(tx->data[i] == 0x33);

    __builtin_memset(tx_storage[0], 0, sizeof(tx_storage[0]));
    rx->data[0] = sequence[508];
    for (unsigned i = 1; i < 700; i++) rx->data[i] = sequence[(508 + i) % 511];
    tx->len = 700;
    test_dut.right_packet_cnt = 0;
    check_rx_fill_tx_data(rx, tx, 0, &test_dut);
    for (unsigned i = 0; i < 700; i++) CHECK(tx->data[i] == sequence[(508 + i) % 511]);
    CHECK(test_dut.right_packet_cnt == 1);
}

static void test_analog_and_detach(void)
{
    static const u8 set_start[] = { 1, 2, 3, 4, 7, 8, 9 };
    static const u8 clear_start[] = { 5, 6, 255 };
    for (unsigned i = 0; i < sizeof(set_start); i++) {
        reset_fixture();
        test_dut.scenario = set_start[i];
        test_dut.hopping_mode = 1;
        test_frame.optcntl = 0;
        dut_cfg_analog(&test_lc);
        CHECK((test_frame.optcntl & 1) != 0 && test_dut.start == 1);
    }
    for (unsigned i = 0; i < sizeof(clear_start); i++) {
        reset_fixture();
        test_dut.scenario = clear_start[i];
        test_dut.hopping_mode = 1;
        test_frame.optcntl = 1;
        dut_cfg_analog(&test_lc);
        CHECK((test_frame.optcntl & 1) == 0);
        CHECK(test_dut.start == (clear_start[i] == 255 ? 0 : 1));
    }
    reset_fixture();
    test_dut.scenario = 1;
    test_dut.packet_type = 0x35;
    test_dut.hopping_mode = 0;
    test_dut.tx_fre = 11;
    test_dut.rx_fre = 10;
    test_frame.miscntl = 0xffff;
    dut_cfg_analog(&test_lc);
    CHECK(test_dut.packet_group == 3 && test_dut.packet_num == 5);
    CHECK(test_dut.packet_len == 540 && test_dut.start == 1);
    CHECK(test_dut.right_packet_cnt == 0 && (test_frame.format & 128) != 0);
    CHECK((test_frame.miscntl & 8) != 0 && dut_reg_1c011c == 685);
    reset_fixture();
    test_dut.scenario = 1;
    test_frame.format = 0xff;
    test_frame.optcntl = 0xffff;
    bredr_dut_detch(&test_lc);
    CHECK(test_dut.scenario == 255 && test_dut.start == 0);
    CHECK(g_dut_hdl == 0 && (test_frame.format & 128) == 0 && (test_frame.optcntl & 1) == 0);
    CHECK((dut_reg_1c0100 & 8) == 0);
}

static void test_clear_callback_reload(void)
{
    struct bredr_tx_bulk first, replacement;
    reset_fixture();
    __builtin_memset(&first, 0, sizeof(first));
    __builtin_memset(&replacement, 0, sizeof(replacement));
    test_lc.tx_bulk[0] = &first;
    test_lc.tx_bulk[1] = &first;
    slot_replacement = &replacement;
    mutate_on_tx_free = 1;
    dut_clear_tx_buf(&test_lc);
    CHECK(tx_free_count == 1 && last_freed_tx == &first);
    CHECK(test_lc.tx_bulk[0] == 0 && test_lc.tx_bulk[1] == 0);
    CHECK(irq_disable_count == 1 && irq_enable_count == 1 && irq_active == 0);
}

static void test_rx_callback_and_null_enqueue(void)
{
    reset_fixture();
    test_heads[0].index = 0;
    test_heads[0].type = 3;
    test_heads[0].llid = 2;
    test_heads[0].len = 1;
    test_heads[0].check = 2;
    test_lc.rx_data[0] = (u8 *)"x";
    test_lc.adjust_clkoffset_enable = 1;
    test_lc.role = 1;
    test_dut.start = 1;
    soft_agc_callback = stop_dut_in_agc;
    dut_rx_handler(&test_lc);
    CHECK(soft_agc_count == 1 && rxoffset_count == 1);
    CHECK(clean_count == 1 && timeout_count == 0);

    reset_fixture();
    test_heads[0].index = 0;
    test_heads[0].type = 3;
    test_heads[0].llid = 3;
    test_heads[0].len = 6;
    test_heads[0].check = 127;
    test_lc.rx_data[0] = (u8 *)"abcdef";
    fail_rx_alloc = 1;
    dut_rx_handler(&test_lc);
    CHECK(rx_alloc_count == 1 && rx_push_count == 1 && last_pushed_rx == 0);
    CHECK(event_count == 1 && timeout_count == 1 && clean_count == 1);
}

static void test_transmit_callback_reloads(void)
{
    struct bredr_tx_bulk done;
    reset_fixture();
    __builtin_memset(&done, 0, sizeof(done));
    done.len = done.offset = 1;
    done.dut_post = 1;
    done.llid = 2;
    test_lc.tx_bulk[0] = &done;
    test_frame.txbuf0cntl = 1;
    test_frame.linkcntl1 = 2;
    test_dut.scenario = 255;
    test_dut.start = 1;
    hci_replacement = &test_other_dut;
    test_other_dut.start = 0;
    dut_rx_handler(&test_lc);
    CHECK(last_lmp_free == &test_other_dut);
    CHECK(last_irq_frame == &test_frame && last_irq_link == &test_lc);
    CHECK(g_dut_hdl == &test_dut && test_lc.tx_bulk[0] == 0);

    reset_fixture();
    __builtin_memset(&done, 0, sizeof(done));
    done.len = done.offset = 1;
    done.enc_post = 4;
    done.unsniff_ack_check = 1;
    test_lc.tx_bulk[0] = &done;
    test_frame.txbuf0cntl = 1;
    test_frame.linkcntl1 = 2;
    encrypt_mutation_tx = &done;
    mutate_event_on_encrypt = 1;
    dut_rx_handler(&test_lc);
    CHECK(encrypt_count == 1 && test_lc.unsniff_ack == 1);
    CHECK(event_count == 0 && tx_free_count == 0);
}

static void test_generated_scenario_reload(void)
{
    reset_fixture();
    test_dut.start = 1;
    test_dut.scenario = 2;
    test_dut.packet_len = 4;
    test_dut.packet_num = 3;
    test_frame.txbuf0cntl = 1;
    test_frame.linkcntl1 = 2;
    mutate_scenario_on_tx_alloc = 1;
    scenario_after_tx_alloc = 4;
    dut_rx_handler(&test_lc);
    CHECK(test_dut.scenario == 4 && tx_alloc_count == 1);
    CHECK(test_lc.tx_bulk[0] == tx_allocations[0]);
    for (int i = 0; i < 4; i++) CHECK(tx_allocations[0]->data[i] == 0);

    static const struct { u8 scenario, value; } fill[] = {
        {1,0},{2,1},{3,0xaa},{9,0x0f}
    };
    for (unsigned t = 0; t < sizeof(fill) / sizeof(fill[0]); t++) {
        reset_fixture();
        test_dut.start = 1;
        test_dut.scenario = fill[t].scenario;
        test_dut.data_len = 4;
        test_dut.packet_len = 8;
        test_frame.txbuf0cntl = 1;
        test_frame.linkcntl1 = 2;
        dut_rx_handler(&test_lc);
        CHECK(tx_alloc_count == 1 && test_lc.tx_bulk[0] == tx_allocations[0]);
        for (int i = 0; i < 4; i++) CHECK(tx_allocations[0]->data[i] == fill[t].value);
    }
}

int main(void)
{
    reset_fixture();
    test_sdk_tables();
    test_prbs_and_loopback_fill();
    test_slot_handshake();
    test_analog_and_detach();
    test_clear_callback_reload();
    test_rx_callback_and_null_enqueue();
    test_transmit_callback_reloads();
    test_generated_scenario_reload();
    return TEST_DONE("btctrler_bredr_dut_bd19");
}
#endif
