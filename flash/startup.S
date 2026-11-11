/*
 * Minimal entry point for the UART-boot RAM stub.
 * The boot ROM jumps here with its own stack; give ourselves a clean one
 * at the top of SRAM0, zero .bss, then run main().
 */
	.syntax unified
	.thumb
	.section .text.start, "ax", %progbits
	.global _start
	.thumb_func
	.type _start, %function
_start:
	cpsid	i
	ldr	r0, =0x00160000
	mov	sp, r0

	ldr	r1, =__bss_start__
	ldr	r2, =__bss_end__
	movs	r3, #0
1:	cmp	r1, r2
	bcs	2f
	str	r3, [r1], #4
	b	1b
2:
	bl	main
3:	b	3b
	.size _start, . - _start
