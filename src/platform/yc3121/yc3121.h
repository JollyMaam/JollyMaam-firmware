#ifndef YC3121_H
#define YC3121_H

#include <stdint.h>

/* Yichip YC3121 registers, from Yichip's SDK (yc3121.h, yc_gpio.c, misc.c)
 * as RT-Thread's bsp/yichip/yc3121-pos carries it. */

/* A pin's byte: 0x3e output low, 0x3f output high, 0x00 input, 0x40 input
 * with pull-up, 0x80 with pull-down; other values pick a peripheral.  Pin n
 * of group g is byte 16 * g + n. */
#define YC_GPIO_CONFIG(g, n) (*(volatile uint8_t *)(0xf8700 + 16 * (g) + (n)))
#define YC_GPIO_IN(g)        (*(volatile uint16_t *)(0xf873c + 2 * (g)))

#define YC_PIN_LOW     0x3e
#define YC_PIN_HIGH    0x3f
#define YC_PIN_PULLUP  0x40

/* The CPU clock: 192 MHz / ((YC_HCLK_CON & 15) + 2). */
#define YC_HCLK_CON    (*(volatile uint32_t *)0xf8560)
/* What the boot loader leaves (divider 2, seen on the pad). */
#define YC_CPU_MHZ     48

/* The Cortex-M0's SysTick, counting down on the CPU clock (CSR bit 2). */
#define YC_SYST_CSR    (*(volatile uint32_t *)0xe000e010)
#define YC_SYST_RVR    (*(volatile uint32_t *)0xe000e014)
#define YC_SYST_CVR    (*(volatile uint32_t *)0xe000e018)

#define YC_RST_EN      (*(volatile uint32_t *)0xf8574)
#define YC_RESET       (*(volatile uint32_t *)0xf857c)

static inline void yc_pin(int group, int pin, int config)
{
    YC_GPIO_CONFIG(group, pin) = config;
}

static inline int yc_pin_in(int group, int pin)
{
    return YC_GPIO_IN(group) >> pin & 1;
}

/* The 8BitDo boot loader's mailbox, read at reset (its 0x01002af8): this
 * word 0x5a5aa5a5 keeps it in boot mode (it clears the word); 0x2fff4 =
 * 0x20210624 sets another flag of its, purpose not established.  Without
 * the word it stays only on L1+R1 or a bad app or version page.  The words
 * survive a jump to the loader, not a chip reset (plat_restart). */
#define YC_LOADER_MAILBOX  (*(volatile uint32_t *)0x2fff0)
#define YC_LOADER_STAY     0x5a5aa5a5
#define YC_LOADER_MAILBOX2 (*(volatile uint32_t *)0x2fff4)
#define YC_LOADER_FROM_APP 0x20210624   /* stock writes it with STAY: the loader
                                         * then skips its radio start (flag 0x200d9) */
/* The loader's mode: with 2 the ROM's fault and interrupt vectors run the
 * loader's handlers (a fault: an endless loop), else the app's (app + 4 +
 * offset; IRQ n at app + 0x9c with r0 = 4n).  Its start of the app after
 * flashing leaves 2, so the app sets 1. */
#define YC_LOADER_MODE     (*(volatile uint32_t *)0xf8564)

/* The chip's LPM register 1 (written as the radio core's are, its ROM's
 * lpm_bt_read at 0x4d25 to read): stock clears its bit 17 first thing at
 * boot (its 0x010272f8).  platform_yc3121.c. */
void plat_lpm_release_pins(void);

/* Busy wait on SysTick (platform_yc3121.c); plat_delay_ms for longer. */
void yc_delay_us(uint32_t us);

/* The chip's reset (the SDK's soft_reset, stock's 0x010288d8): the boot
 * loader runs again (platform_yc3121.c). */
__attribute__((noreturn)) void yc_reset(void);

/* An exception's entry (start.S): records it and has the exception return
 * into plat_restart(0). */
void yc_fault(const uint32_t *frame, uint32_t ipsr);
void yc_systick(uint32_t *frame);
void plat_watch_kick(void);      /* the hang watch: the main loop is alive */

/* Restart through the boot loader; loader: stay there. */
__attribute__((noreturn)) void plat_restart(int loader);

#endif
