/*
 * Linker script for the guarded flash writer (mw300_flashwr), load base
 * 0x00110000.  The host appends the parameter block at +0x0FC0 and the
 * payload at +0x1000 (FW_PARAM_OFF / FW_PAYLOAD_OFF in flashwr_core.h), so
 * code and data must end below 0x0FC0.  With the default 4 KB payload the
 * whole ROM load is 0x2000 bytes, the size proven on hardware (0x2004).
 *
 * .bss (read buffers, the runtime vector table) lives at 0x00150000, outside
 * the load image and far above the largest payload (64 KB, ending at
 * 0x00121000); startup.S zeroes it.  The stack is at 0x00160000.
 */
ENTRY(_start)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x00110000, LENGTH = 0x0FC0
	BSS (rw)  : ORIGIN = 0x00150000, LENGTH = 0x4000
}

SECTIONS
{
	.text : {
		KEEP(*(.text.start))
		*(.text*)
		*(.rodata*)
	} > RAM

	.data : {
		*(.data*)
	} > RAM

	__image_end__ = .;

	.bss (NOLOAD) : {
		. = ALIGN(128);
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end__ = .;
	} > BSS

	/DISCARD/ : {
		*(.ARM.exidx*)
		*(.ARM.extab*)
	}

	ASSERT(__image_end__ <= ORIGIN(RAM) + 0x0FC0,
	       "flash writer must end below its parameter block at +0x0FC0")
	ASSERT(SIZEOF(.data) == 0, "no initialised .data expected")
}
