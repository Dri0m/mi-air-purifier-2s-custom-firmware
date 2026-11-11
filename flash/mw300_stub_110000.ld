/*
 * Linker script for the UART-boot RAM stub.
 *
 * Code RAM starts at 0x00100000 and the boot ROM reserves none of SRAM0
 * (only 0x20000000-0x20005657 in SRAM1).  The loader also writes a 4-byte
 * dump length at 0x00112000 (base + 0x2000), so everything here must fit below that.
 */
ENTRY(_start)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x00110000, LENGTH = 0x50000
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

	.bss : {
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		__bss_end__ = .;
	} > RAM

	/DISCARD/ : {
		*(.ARM.exidx*)
		*(.ARM.extab*)
	}

	ASSERT(SIZEOF(.text) + SIZEOF(.data) + SIZEOF(.bss) < 0x2000,
	       "stub must fit below the dump-length word at base + 0x2000")
}
