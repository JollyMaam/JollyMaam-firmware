/* BD19 interrupt pending bits: eight four-bit IRQ slots per status word.
 * SDK interrupt.c.o uses bit zero of each slot and one volatile read. */
#include "btctrler/glue.h"

/* Keep the SDK i8 return ABI despite the recovered bool declaration. */
u8 irq_get_status_ir(u32 index) __asm__("irq_get_status");
u8 irq_get_status_ir(u32 index)
{
    volatile u32 *status = (volatile u32 *)(uintptr_t)0x1be100;
    return (status[index >> 3] & (1u << ((index << 2) & 28))) != 0;
}
