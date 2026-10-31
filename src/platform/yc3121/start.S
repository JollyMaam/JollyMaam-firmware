/* Yichip YC3121 app entry, at the app slot's base (0x01018000 on the 8BitDo
 * Ultimate 2C Bluetooth).  The boot loader jumps to the base; exceptions and
 * interrupts arrive at fixed offsets from it, the layout stock v1.01 and
 * Yichip's SDK (flash_start.s) share: reset at 0, hard fault 4, SVC 0xa,
 * PendSV 0x10, SysTick 0x16 (6 bytes each), then IRQ n at 0x1c + 4n.
 * Nothing here enables an interrupt; every entry but reset goes to
 * yc_fault(), which records it, and returns from the exception into a
 * restart through the boot loader.  The vectors are the ROM's (a Cortex-M0
 * has no VTOR): HardFault runs the ROM's handler, which resets the chip if
 * OTP word 0 is 0x7777 (and 0xf853c bit 0 clear), else calls the loader's
 * stub, which comes here when the loader's mode (0xf8564) is not 2. */

	.syntax unified
	.cpu cortex-m0
	.thumb

	.section .entry, "ax"
	.global _entry
	.thumb_func
_entry:
	ldr r0, =reset
	bx r0
	.rept 4                         /* hard fault, SVC, PendSV, SysTick */
	ldr r0, =trap
	bx r0
	nop
	.endr
	.rept 32                        /* IRQ 0..31 */
	b trap_near
	nop
	.endr
	.thumb_func
trap_near:
	ldr r0, =trap
	bx r0
	.ltorg

	.text
	.thumb_func
reset:
	cpsid i
	ldr r0, =__stack_top
	mov sp, r0
	ldr r1, =__data_load            /* .data from flash */
	ldr r2, =__data_start
	ldr r3, =__data_end
1:	cmp r2, r3
	bhs 2f
	ldr r0, [r1]
	str r0, [r2]
	adds r1, #4
	adds r2, #4
	b 1b
2:	ldr r2, =__bss_start
	ldr r3, =__bss_end
	movs r0, #0
3:	cmp r2, r3
	bhs 4f
	str r0, [r2]
	adds r2, #4
	b 3b
4:	bl main
	.thumb_func
trap:                                   /* yc_fault(the stacked frame, IPSR) */
	mrs r0, msp
	mrs r1, ipsr
	cmp r1, #3                              /* HardFault: the ROM's handler (0x11c) */
	bne 1f                                  /* pushed r4, lr and called us through */
	adds r0, #8                             /* the loader: the frame is above them */
1:	push {r0, lr}                           /* lr: back to the ROM, or the exception return */
	bl yc_fault
	pop {r0, pc}
	.ltorg
