#include <system/includes.h>
#include <asm/dac.h>
#include "board.h"

/* The DAC and the headphone amplifier (docs/audio.md), fed with the host's
 * 48 kHz stereo PCM from the USB audio endpoint.
 *
 * The DAC runs, playing silence, while a USB personality with audio is up:
 * starting and stopping it pops.  The amplifier follows the host's stream,
 * as on the stock pad.  Packets go straight into the DAC's ring; a timer
 * tops it up with silence when the host stops sending.  The ring holds about
 * TARGET frames: one frame more or less per packet keeps it there when the
 * host's clock and the DAC's drift apart. */

#define RING        1024         /* frames */
#define TARGET      (48 * 8)     /* 8 ms */
#define SLACK       (48 * 2)
#define LOW         (48 * 2)     /* the timer's silence keeps at least this */
#define TOP_UP_MS   2
#define ANALOG_GAIN 24           /* of 31; the host's volume is digital */
#define FULL_GAIN   16384

static struct audio_dac_hdl dac;
static struct dac_platform_data dac_data = {
    .output = DAC_OUTPUT_LR, .ldo_volt = DACVDD_LDO_2_70V,
    .ldo_isel = 3, .ldo_fb_isel = 2, .lpf_isel = 0x8,
};
static s16 ring[2 * RING] __attribute__((aligned(4)));
static u8 inited, running;
static volatile u8 streaming;
static u16 timer;
static s16 last[2];

static void amplifier(int on)
{
    gpio_direction_output(board_amp_enable, on);
}

static u32 room(void)
{
    return JL_AUDIO->DAC_SWN;    /* reads back the free frames */
}

/* Interrupts off: the USB interrupt and the timer both write. */
static void put(const s16 *frames, u32 n)
{
    while (n) {
        u32 swp = JL_AUDIO->DAC_SWP, k = room();

        if (swp + k > JL_AUDIO->DAC_LEN) k = JL_AUDIO->DAC_LEN - swp;
        if (k > n) k = n;
        if (!k) return;
        if (frames) memcpy(ring + 2 * swp, frames, k * 4);
        else memset(ring + 2 * swp, 0, k * 4);
        JL_AUDIO->DAC_SWN = k;
        __asm_csync();
        if (frames) frames += 2 * k;
        n -= k;
    }
}

static void top_up(void *arg)
{
    u32 held;

    (void)arg;
    local_irq_disable();
    held = RING - room();
    if (held < LOW) put(NULL, TARGET - held);
    local_irq_enable();
}

void plat_audio_write(const u8 *pcm, u32 len)
{
    const s16 *f = (const s16 *)pcm;
    u32 n = len / 4, held;

    if (!running || !n) return;
    local_irq_disable();
    held = RING - room();
    if (held + n > TARGET + SLACK) n--;          /* the DAC is slower: drop one */
    put(f, n);
    last[0] = f[2 * n - 2];
    last[1] = f[2 * n - 1];
    if (held + n < TARGET - SLACK) put(last, 1); /* faster: repeat one */
    local_irq_enable();
}

/* Attenuation in 1/256 dB to the DAC's linear gain: a table for the
 * half-dB steps of one 6 dB octave, then halving per 6 dB (6.02 dB really;
 * 0.2 dB off at -60). */
static u32 gain(s16 db)
{
    static const u16 octave[12] = {    /* FULL_GAIN * 10^(-k/40) */
        16384, 15467, 14602, 13785, 13014, 12286,
        11599, 10950, 10338,  9759,  9213,  8698,
    };
    u32 att = (u32)(db < 0 ? -db : 0) / 128;     /* half dB steps */

    return octave[att % 12] >> (att / 12);
}

void plat_audio_volume(s16 left, s16 right, u8 mute)
{
    u32 l = mute ? 0 : gain(left);
    u32 r = mute ? 0 : gain(right);

    if (!running) return;
    audio_dac_ch_digital_gain_set(&dac, BIT(0), l);
    audio_dac_ch_digital_gain_set(&dac, BIT(1), r);
}

/* From the USB interrupt: the host opened or closed its stream. */
void plat_audio_stream(int on)
{
    streaming = (u8)on;
    if (!running) return;
    if (on) {
        local_irq_disable();
        put(NULL, TARGET - (RING - room() < TARGET ? RING - room() : TARGET));
        local_irq_enable();
    }
    amplifier(on);
}

/* Control thread.  The first time ever it trims the DAC (about 0.8 s) and
 * keeps the result. */
void plat_audio_on(void)
{
    static struct audio_dac_trim trim;

    if (running) return;
    amplifier(0);
    if (!inited) {
        audio_dac_init(&dac, &dac_data);
        audio_dac_set_buff(&dac, ring, sizeof(ring));
        if (syscfg_read(CFG_DAC_TRIM_INFO, &trim, sizeof(trim)) != sizeof(trim) ||
            !trim.left || !trim.right) {
            audio_dac_do_trim(&dac, &trim, 0);
            syscfg_write(CFG_DAC_TRIM_INFO, &trim, sizeof(trim));
        }
        audio_dac_set_trim_value(&dac, &trim);
        audio_dac_set_delay_time(&dac, 1, 2);
        inited = 1;
    }
    audio_dac_set_sample_rate(&dac, 48000);
    audio_dac_start(&dac);
    JL_AUDIO->DAC_CON |= BIT(6) | BIT(5);
    audio_dac_ch_analog_gain_set(&dac, 0x3, ANALOG_GAIN);
    audio_dac_ch_digital_gain_set(&dac, 0x3, 0);
    JL_AUDIO->DAC_CON &= ~(BIT(12) | BIT(13) | BIT(14) | BIT(15));
    local_irq_disable();
    put(NULL, TARGET);
    running = 1;
    local_irq_enable();
    timer = usr_timer_add(NULL, top_up, TOP_UP_MS, 1);
    util_log("audio: on, trim %d %d", trim.left, trim.right);
}

/* Amplifier first, then the DAC: no pop at a reset or power-off. */
void plat_audio_off(void)
{
    if (!running) return;
    amplifier(0);
    running = 0;
    streaming = 0;
    if (timer) usr_timer_del(timer);
    timer = 0;
    plat_mdelay(50);
    audio_dac_stop(&dac);
    util_log("audio: off");
}
