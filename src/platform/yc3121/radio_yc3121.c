#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "radio_yc3121.h"

/* The YC3121's Bluetooth core: a second processor behind a window at
 * 0xc0000..0xc8fff, started and fed as stock v1.01 does (its 0x01020d58,
 * 0x010269e4, the IPC at 0x01020844..0x01020d18).  Its program is Yichip's
 * blob (from the stock app, built in by make; not in this repository):
 * u16, u16 0x55aa, u16 code length, the code (streamed into a port), then
 * records {u16 0x55aa, u16 length, u16 address, data} for 0xc0000 + address.
 *
 * IPC: two byte rings in the window, each described by four LE16 offsets
 * from 0xc0000 {first, last, read, write}: the core's packets to us at
 * 0xc4ff0, ours to it at 0xc4ff8 (the blob's records put them there).  A
 * packet is {type, id, u8 length, data} (ours type 1; the core's type 2);
 * some of ours (stock: 0xa0, 0xb0) take a u16 length.  Bit 7 of 0xc8043 is the core's
 * interrupt (IRQ 11, which stays off here: we poll); writing it 0 clears
 * it. */

#define WINDOW(off)    (*(volatile uint8_t *)(0xc0000 + (off)))
#define BT_RESET       (*(volatile uint8_t *)0xc8010)
#define BT_IRQ         (*(volatile uint8_t *)0xc8043)
#define BT_LPM_DATA(n) (*(volatile uint8_t *)(0xc804c + (n)))
#define BT_LPM_WRITE   (*(volatile uint8_t *)0xc8005)
#define BT_CODE_CTL(n) (*(volatile uint8_t *)(0xc8020 + (n)))
#define BT_CODE_PORT   0xc8025
#define WAKEUP_BT      (*(volatile uint8_t *)0xf853c)
#define NVIC_ICER      (*(volatile uint32_t *)0xe000e180)
#define CLK_CLS        (*(volatile uint32_t *)0xf856c)
#define CLK_BT         (1u << 13)

#define DMA6(off)      (*(volatile uint32_t *)(0xf8e00 + (off)))
#define DMA6_MODE      (*(volatile uint8_t *)0xf8e0c)
#define DMA6_START     (*(volatile uint8_t *)0xf8e0f)

#define RX_RING 0x4ff0
#define TX_RING 0x4ff8

/* The ROM's LPM read for the radio (Yichip's rom_api.h: lpm_bt_read). */
#define ROM_LPM_BT_READ ((uint32_t (*)(int))0x4d25)

extern const uint8_t radio_blob[], radio_blob_end[];

static void wait_ms(u32 ms)
{
    plat_delay_ms(ms);
}

/* DMA channel 6, as stock copies into the window (and into the code port,
 * with mode 8: the destination stays). */
static void dma6(uint32_t dst, const void *src, uint32_t n)
{
    DMA6(0) = (uint32_t)src;
    DMA6(4) = dst;
    DMA6(8) = n << 16 | n;
    DMA6_START = 0x80;
    while (!(DMA6(0x10) & 1))
        ;
}

static void lpm_bt_write(int index, uint32_t value)
{
    CLK_CLS &= ~CLK_BT;
    for (int i = 0; i < 4; i++) {
        BT_LPM_DATA(i) = (uint8_t)(value >> 8 * i);
        yc_delay_us(1);
    }
    BT_LPM_WRITE = (uint8_t)(1u << index);
    plat_delay_ms(1);
}

static int load(const uint8_t *blob, const uint8_t *end)
{
    uint16_t code;

    if (end - blob < 6 || (blob[2] | blob[3] << 8) != 0x55aa) return 0;
    code = (uint16_t)(blob[4] | blob[5] << 8);
    CLK_CLS &= ~CLK_BT;
    BT_CODE_CTL(4) = 0;
    BT_CODE_CTL(2) = 0;
    BT_CODE_CTL(3) = 0x80;
    DMA6_MODE = 8;
    dma6(BT_CODE_PORT, blob + 6, code);
    BT_CODE_CTL(3) = 0;
    DMA6_MODE = 0;
    for (const uint8_t *p = blob + 6 + code; p + 6 <= end && (p[0] | p[1] << 8) == 0x55aa;) {
        uint16_t n = (uint16_t)(p[2] | p[3] << 8);
        uint32_t at = 0xc0000 + (uint32_t)(p[4] | p[5] << 8);
        uint8_t word[4] = { 0 };

        if (at == 0xc453d && n == 4) {          /* as stock: 1, then data[1..3] */
            word[0] = 1;
            memcpy(word + 1, p + 7, 3);
            dma6(at, word, 4);
        } else if (at == 0xc4acf && n == 1) {   /* as stock: 0 */
            dma6(at, word, 1);
        } else {
            dma6(at, p + 6, n);
        }
        p += 6 + n;
    }
    return 1;
}

static u8 adc_up;                /* plat_adc_init() since the core last started */

int plat_adc_ready(void)
{
    return adc_up;
}

int plat_radio_start(void)
{
    u32 t0;

    adc_up = 0;
    NVIC_ICER = 1u << 11;
    WAKEUP_BT = 0x94;
    wait_ms(10);
    BT_RESET = 1;
    wait_ms(10);
    for (t0 = plat_ms(); !(BT_IRQ & 0x80);)
        if (plat_ms() - t0 > 500) return -1;
    BT_IRQ &= 0x7f;
    lpm_bt_write(1, ROM_LPM_BT_READ(1) | 0xd00000);
    lpm_bt_write(0, 0x3084cf0f);
    for (uint32_t i = 0; i < 0x1000; i++) {
        WINDOW(i) = 0;
        yc_delay_us(1);
        WINDOW(0x4000 + i) = 0;
        yc_delay_us(1);
    }
    if (!load(radio_blob, radio_blob_end)) return -2;
    WAKEUP_BT = 0x9c;
    wait_ms(100);
    return 0;
}

static uint16_t ring(uint32_t desc, int k)
{
    return (uint16_t)(WINDOW(desc + 2 * k) | WINDOW(desc + 2 * k + 1) << 8);
}

static uint16_t next(uint32_t desc, uint16_t at)
{
    return at == ring(desc, 1) ? ring(desc, 0) : (uint16_t)(at + 1);
}

static uint32_t room(void)
{
    uint16_t r = ring(TX_RING, 2), w = ring(TX_RING, 3);

    return w < r ? (uint32_t)(r - w) : (uint32_t)(ring(TX_RING, 1) - ring(TX_RING, 0) + 1 - (w - r));
}

int plat_radio_send(u8 id, const u8 *data, u16 len, int wide)
{
    u8 head[4] = { 1, id, (u8)len, (u8)(len >> 8) };
    int h = wide ? 4 : 3;
    uint16_t w;

    if ((!wide && len > 255) || room() < (uint32_t)(h + len)) return 0;
    w = ring(TX_RING, 3);
    for (int i = 0; i < h + len; i++) {
        WINDOW(w) = i < h ? head[i] : data[i - h];
        w = next(TX_RING, w);
    }
    WINDOW(TX_RING + 6) = (uint8_t)w;
    WINDOW(TX_RING + 7) = (uint8_t)(w >> 8);
    return 1;
}

int plat_radio_take(struct radio_packet *p)
{
    uint16_t r = ring(RX_RING, 2);

    if (BT_IRQ & 0x80) BT_IRQ &= 0x7f;
    if (r == ring(RX_RING, 3)) return 0;
    p->type = WINDOW(r), r = next(RX_RING, r);
    p->id = WINDOW(r), r = next(RX_RING, r);
    p->len = WINDOW(r), r = next(RX_RING, r);
    for (int i = 0; i < p->len; i++) {
        p->data[i] = WINDOW(r);
        r = next(RX_RING, r);
    }
    WINDOW(RX_RING + 4) = (uint8_t)r;
    WINDOW(RX_RING + 5) = (uint8_t)(r >> 8);
    return 1;
}

/* The ADC, behind the same window (stock v1.01's 0x01018bf8 and 0x010189dc,
 * Yichip's yc_adc.c): it works once the core is powered (plat_radio_start).
 * Pins 2.5, 2.6, 2.8, 2.9 and 2.10 analog (config 0xc0), as stock sets them;
 * mode 0 (bits 4-6 of 0x11); a channel's conversion is ready 100 us after
 * its select, an 11-bit value in bits 1-10 of 0xf850e.  Stock reads its
 * sticks on channels 0, 1, 4, 5 (0x01025ef0) and the battery on 3; it
 * checks the 0 V value on 3 with pin 2.8 driven low (0x01018868). */
#define ADC(off)       (*(volatile uint8_t *)(0xc8960 + (off)))
#define ADC_RESULT     (*(volatile uint16_t *)0xf850e)

void plat_adc_init(void)
{
    static const uint8_t pins[] = { 5, 6, 8, 9, 10 };

    for (unsigned i = 0; i < sizeof(pins); i++) yc_pin(2, pins[i], 0xc0);
    ADC(0x11) = 0x8e;
    ADC(0x10) = 0x7f;
    ADC(0x12) = 0xaa;
    ADC(0x13) = 0x30;
    plat_delay_ms(100);
    adc_up = 1;
}

u16 plat_adc_read(int channel)
{
    ADC(0x10) = 0x7f;
    ADC(0x12) = 0xaa;
    ADC(0x13) = (uint8_t)(channel | 0x30);
    yc_delay_us(100);
    return ADC_RESULT & 0x7fe;
}
