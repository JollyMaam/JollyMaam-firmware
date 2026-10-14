#ifndef BOARD_H
#define BOARD_H

/* Between platform_jieli.c (the chip, the SDK, USB, the tick) and the board
 * file linked with it: board_pad.c (the Cyclone 2) or board_dongle.c (the
 * GameSir dongle).  Not seen by the rest of the firmware. */

#include "pad.h"

#define HOT __attribute__((section(".volatile_ram_code")))
#define TICK_US 250

struct wakeup_param;

/* A button: active low, pulled up, debounced in the tick. */
struct line { u32 gpio; u16 bit; };

/* ---- the board ---------------------------------------------------------- */

extern const struct line board_lines[];
extern const u8 board_line_count;        /* at most 32 */
extern const u32 board_keep_pins[];      /* left alone at power-off */
extern const u8 board_keep_count;
extern const struct wakeup_param *const board_wakeups;   /* NULL: none */
extern const u8 board_audio;             /* a DAC and headphone amplifier */
extern const u32 board_amp_enable;       /* the amplifier, on while high */

void board_latch(void);          /* first in board_init: what is held; may enter the loader */
void board_start(void);          /* on the control thread, before the tick starts */
void board_tick(void);           /* every tick, in its interrupt */
void board_to_loader(void);      /* about to restart into the loader */
int board_usb_sense(void);       /* 5 V on the USB connector */

/* ---- the platform, for the board ----------------------------------------- */

extern struct plat_stats plat_counters;   /* written with interrupts off or in one */
extern volatile u32 plat_now_ms;

u32 plat_read_lines(void);               /* now, not debounced: bit i is board_lines[i] */
u32 plat_lines(u32 *changed_us);         /* debounced */
void plat_raise(u32 ev);                 /* from an interrupt */
void plat_mdelay(u32 ms);                /* busy wait, interrupts may be off */
void plat_flush_dcache(const void *address, u32 size);
void plat_enter_loader(void);            /* now, without detaching USB first */
void plat_pull_up_input(u32 gpio);

/* ---- audio, between the platform's files (platform_jieli_audio.c) --------- */

void plat_audio_on(void);                /* DAC running silent, amplifier off */
void plat_audio_off(void);               /* amplifier off, then the DAC */
void plat_audio_stream(int on);          /* the host's stream; from an interrupt */
void plat_audio_write(const u8 *pcm, u32 len);   /* 48 kHz stereo s16; from an interrupt */
void plat_audio_volume(u8 left, u8 right, u8 mute);   /* 0 to 100 */

#endif
