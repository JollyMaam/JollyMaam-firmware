/*
 * Baseband transmit for the Switch 1 wake sequence.  See wake_radio.h.
 *
 * The register sequence is the one the vendor's own fixed-frequency transmit
 * path uses (__fix_txrx_enable() with its transmit role, in bredr_link.c.o of
 * the controller archive).  That code is known to put a real burst on a
 * chosen channel with a chosen access code, so its ordering and its constants
 * are reproduced rather than invented.  The three departures from it are the
 * point of this file: the parity comes from the wake-up code instead of the
 * address, the payload is ours instead of a repeated fill byte, and the burst
 * interval is the wake cadence instead of the test cadence.
 */

#include "generic/typedef.h"

#include "wake_radio.h"
#include "main.h"
#include "usb.h"

/* 0x9Axx markers in the bring-up ring: one per step of wake_radio_start(),
 * 0x9A20/0x9A21 for every scheduler resume/suspend, so a reset during the
 * wake names the step it died in (tools/ringdump.py). */
#define WAKE_MARK(n) cyclone2_bringup_record(0x9a00u | (n))

/* ---- the vendor symbols this uses ----------------------------------------
 *
 * Declared here because the controller archive ships no headers.  The same
 * pattern (and the same reason) as the lmp_set_sniff_establish_by_remote()
 * declaration in src/bt/btstack_jl_sdk.c.
 */
struct ctrl_frame;

struct link_task;

struct link_task_ops {
    int (*resume)(struct link_task *task);
    int (*suspend)(struct link_task *task);
};

struct ctrl_frame *bredr_bd_get_frame(void);
void bredr_bd_put_frame(struct ctrl_frame *frame);
void bredr_bd_frame_enable(struct ctrl_frame *frame, u8 flag, u8 set_en);
void bredr_bd_frame_disable(struct ctrl_frame *frame);

void __write_reg_bdaddr(struct ctrl_frame *frame, const u8 *address);
void __write_reg_bch(struct ctrl_frame *frame, const u8 *bch);
void __write_reg_format(struct ctrl_frame *frame, u8 train, u8 format);
void __write_reg_miscntl(struct ctrl_frame *frame, u8 ptt, u8 lt_addr);
void __write_reg_linkcntl(struct ctrl_frame *frame, int index, int value);
void __write_reg_clkoffset(struct ctrl_frame *frame, int clk_offset);
void __write_reg_txinfo(struct ctrl_frame *frame, int index, u8 *data,
                        int length, int llid, int flow, int packet);
void __clean_reg_txempty(struct ctrl_frame *frame, int index);

u16 *get_packet_type(int length, u8 edr, u8 slot);

/* A packet buffer owned by the controller library, so that it lands in the
 * window the baseband can address.  A plain static array does not: the
 * archive's own writer resets the chip when the buffer is outside that
 * window, which is why the vendor allocates these. */
struct bredr_tx_bulk;
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size);
void bredr_tx_bulk_free(struct bredr_tx_bulk *bulk);

/* The scheduler that decides when a frame is allowed to transmit.  run_slot
 * is the period in slots of 625 us. */
int link_task_idle_disable(void);
void link_task_add(struct link_task *task);
void link_task_del(struct link_task *task);

/* ---- the register block ------------------------------------------------
 *
 * Field offsets in bytes, read out of the archive's DWARF for struct
 * ctrl_frame (bredr_frame.c.o, source lines 28..108).  Only the fields with
 * no writer helper are touched through these; everything else goes through
 * __write_reg_*.  Accessing them through a byte pointer keeps this free of
 * struct-aliasing assumptions about a type we cannot name.
 */
#define WAKEF_WINCNTL   16U  /* u16, whitening control */
#define WAKEF_TXFREQ    21U  /* u8, transmit channel */
#define WAKEF_RXFREQ    23U  /* u8, receive channel */
#define WAKEF_BUFCTL    88U  /* u16, buffer control */

/* Layout of struct link_task, same source.  `entry` is a two-pointer list
 * node the scheduler links in; `ops` is what arms and disarms the frame. */
#define WAKETASK_SIZE   28U
#define WAKETASK_OFF_ATTR 0U
#define WAKETASK_OFF_NUM  3U
#define WAKETASK_OFF_PRIO 4U
#define WAKETASK_OFF_TYPE 6U
#define WAKETASK_OFF_SLOT 8U
#define WAKETASK_OFF_OPS  24U

/* Payload lives directly behind the 28-byte header of a bredr_tx_bulk. */
#define WAKE_TX_BULK_PAYLOAD 28U

/* Values the vendor's transmit path uses.  LINKCNTL is split into two words
 * because the hardware has one per buffer. */
#define WAKE_FRAME_FORMAT    2U
#define WAKE_LINKCNTL0   16401U
#define WAKE_LINKCNTL1   16385U
#define WAKE_TASK_TYPE    4096U
#define WAKE_LT_ADDR         7U

/* The body a wake burst carries is under 340 bytes, so one packet is enough;
 * the length is still passed through get_packet_type() so the packet code
 * matches the length the way the vendor's path does. */
#define WAKE_PACKET_SLOT     5U

static struct ctrl_frame *wake_frame;
static struct bredr_tx_bulk *wake_bulk;
static u8 wake_task_storage[WAKETASK_SIZE] __attribute__((aligned(4)));
static u8 wake_task_running;

static volatile u8 *frame_reg(struct ctrl_frame *frame, unsigned offset)
{
    return (volatile u8 *)frame + offset;
}

/* The controller indexes channels through its own frequency table: even
 * channels occupy the first half of the table and odd channels the second. */
static u8 wake_frequency(u8 channel)
{
    u8 index = (u8)(channel >> 1);

    if (channel & 1U) {
        index = (u8)(index + 40U);
    }
    return index;
}

static int wake_task_resume(struct link_task *task)
{
    (void)task;
    WAKE_MARK(0x20);
    if (wake_frame) {
        bredr_bd_frame_enable(wake_frame, 0, 1);
        wake_task_running = 1U;
    }
    return 0;
}

static int wake_task_suspend(struct link_task *task)
{
    (void)task;
    WAKE_MARK(0x21);
    if (wake_frame) {
        bredr_bd_frame_disable(wake_frame);
        wake_task_running = 0U;
    }
    return 0;
}

static const struct link_task_ops wake_task_ops = {
    wake_task_resume,
    wake_task_suspend,
};

int wake_radio_active(void)
{
    return wake_frame != NULL;
}

int wake_radio_start(const struct wake_radio_config *config, const u8 *body, u16 length)
{
    u16 *packet;
    u16 max_length;
    u8 *payload;
    u8 *task;
    u16 control;

    if (config == NULL || body == NULL || length == 0U) return -1;
    if (wake_frame != NULL) return -1;

    WAKE_MARK(0x01);
    wake_frame = bredr_bd_get_frame();
    if (wake_frame == NULL) return -2;

    WAKE_MARK(0x02);
    wake_bulk = bredr_tx_bulk_alloc((int)length);
    if (wake_bulk == NULL) {
        bredr_bd_put_frame(wake_frame);
        wake_frame = NULL;
        return -3;
    }
    payload = (u8 *)wake_bulk + WAKE_TX_BULK_PAYLOAD;
    memcpy(payload, body, length);

    /* Access code: LAP and parity are separate fields, so this is the one
     * place the wake-up access code can be produced. */
    WAKE_MARK(0x03);
    __write_reg_bdaddr(wake_frame, config->address);
    __write_reg_bch(wake_frame, config->parity);

    /* Frame shape, matching the vendor transmit path. */
    WAKE_MARK(0x04);
    __write_reg_miscntl(wake_frame, 0U, WAKE_LT_ADDR);
    __write_reg_clkoffset(wake_frame, 0);
    __write_reg_format(wake_frame, 0U, WAKE_FRAME_FORMAT);
    __write_reg_linkcntl(wake_frame, 0, (int)WAKE_LINKCNTL0);
    __write_reg_linkcntl(wake_frame, 1, (int)WAKE_LINKCNTL1);
    *((volatile u16 *)(void *)frame_reg(wake_frame, WAKEF_BUFCTL)) &= (u16)~0x0f00U;
    *((volatile u16 *)(void *)frame_reg(wake_frame, WAKEF_BUFCTL)) |= (u16)0x0100U;

    /* One burst of this body.  get_packet_type() answers with the packet whose
     * maximum payload covers the length, so a short body becomes a DM1 and a
     * long one a DH3 or DH5, exactly as the vendor's path chooses. */
    WAKE_MARK(0x05);
    packet = get_packet_type((int)length, config->edr, WAKE_PACKET_SLOT);
    max_length = packet[0];
    if (length > max_length) length = max_length;
    {
        /* __write_reg_txinfo() resets the chip (cpu_reset) unless the payload
         * lies within 32 KB above the baseband's buffer base, which the
         * hardware holds at 0x1CFC44.  Check it here and fail the start
         * instead, with the numbers on the console. */
        u32 base = *(volatile u32 *)0x1cfc44u;
        u32 delta = (u32)payload - base;

        cyclone2_usb_debug("wake: bb base=%08x bulk=%08x payload=%08x delta=%x len=%u",
                           (unsigned)base, (unsigned)(u32)wake_bulk,
                           (unsigned)(u32)payload, (unsigned)delta, length);
        if (delta >= 32767U) {
            WAKE_MARK(0x40);
            bredr_tx_bulk_free(wake_bulk);
            wake_bulk = NULL;
            bredr_bd_put_frame(wake_frame);
            wake_frame = NULL;
            return -4;
        }
    }
    WAKE_MARK(0x06);
    __write_reg_txinfo(wake_frame, 0, payload, (int)length,
                       (int)config->llid, (int)config->flow, (int)packet[1]);
    WAKE_MARK(0x07);
    __clean_reg_txempty(wake_frame, 0);
    __clean_reg_txempty(wake_frame, 1);

    control = *((volatile u16 *)(void *)frame_reg(wake_frame, WAKEF_BUFCTL));
    control &= (u16)~0x0020U;
    control &= (u16)~0x0002U;
    control |= (u16)0x0001U;
    *((volatile u16 *)(void *)frame_reg(wake_frame, WAKEF_BUFCTL)) = control;

    /* Whitening: the wake body must reach the air unwhitened, so the caller
     * pre-cancelled it with this seed. */
    *((volatile u16 *)(void *)frame_reg(wake_frame, WAKEF_WINCNTL)) =
        (u16)config->whiten_seed;

    WAKE_MARK(0x08);
    *frame_reg(wake_frame, WAKEF_TXFREQ) = wake_frequency(config->channel);
    *frame_reg(wake_frame, WAKEF_RXFREQ) = wake_frequency(config->channel);

    task = (u8 *)wake_task_storage;
    memset(task, 0, WAKETASK_SIZE);
    task[WAKETASK_OFF_ATTR] = 1U;
    task[WAKETASK_OFF_NUM] = 1U;
    task[WAKETASK_OFF_PRIO] = 1U;
    *((u16 *)(void *)(task + WAKETASK_OFF_TYPE)) = WAKE_TASK_TYPE;
    *((u16 *)(void *)(task + WAKETASK_OFF_SLOT)) = config->interval_slots;
    *((const struct link_task_ops **)(void *)(task + WAKETASK_OFF_OPS)) = &wake_task_ops;

    /* The idle task owns the schedule otherwise, and the frame is only
     * transmitted when this task's ops arm it. */
    WAKE_MARK(0x09);
    link_task_idle_disable();
    WAKE_MARK(0x0a);
    link_task_add((struct link_task *)task);
    WAKE_MARK(0x0b);
    return 0;
}

void wake_radio_stop(void)
{
    if (wake_frame == NULL) return;

    link_task_del((struct link_task *)wake_task_storage);
    if (wake_task_running) {
        /* The task was armed, so the radio was enabled for this frame; the
         * frame's own disable is what turns that off. */
        bredr_bd_frame_disable(wake_frame);
        wake_task_running = 0U;
    }
    bredr_bd_put_frame(wake_frame);
    wake_frame = NULL;

    if (wake_bulk) {
        bredr_tx_bulk_free(wake_bulk);
        wake_bulk = NULL;
    }
}
