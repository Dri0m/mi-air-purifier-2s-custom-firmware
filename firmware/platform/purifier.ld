/*
 * purifier.ld - appfw-slot layout for the Mi Air Purifier 2S (88MW300).
 *
 * Derived from the vendor SDK's build/toolchains/arm_gcc/mw300-xip.ld
 * (Copyright (C) 2008-2019, Marvell International Ltd. All Rights Reserved;
 * distributed in the Amazon FreeRTOS repository), which is the layout
 * the stock Xiaomi app uses (checked against appfwA: seg0 SRAM0 0x00100000
 * at file 0xC8, seg1 XIP 0x1F0033E0 at file 0x33E0, seg2 SRAM1 0x20000000).
 *
 * Image (built by tools/mrvl_pack.py):
 *   file 0x000  MRVL header + segment table, padded to 0xC8 (_text_offset)
 *   file 0x0C8  .init  -> SRAM0 0x00100000 (vectors + flash/QSPI/clock/PMU
 *                         code that must not run from XIP), copied by boot2
 *   file 0xC8+SIZEOF(.init)
 *               .text  -> XIP 0x1F000000 + same offset (runs in place; the
 *                         active slot is mapped at 0x1F000000 by boot2)
 *   then        .data  -> SRAM1 0x20000000, copied by boot2 (no copy in the
 *                         reset handler, so LMA == VMA)
 *
 * boot2 itself runs from SRAM0 0x0015C000..0x0015FFFF while loading us, so
 * loadable segments must stay below 0x0015C000 (mrvl_pack.py checks it).
 * NOLOAD regions (heap, keystore) may cover it: they are only used after
 * boot2 has jumped to us.
 */

ENTRY(Reset_IRQHandler)

_estack = 0x20020000;            /* end of SRAM1 */
_heap_size = (72 * 1024);        /* minimum total heap, asserted below */
_text_offset = 0xc8;             /* = mrvl_pack.py --header-size */
_flashc_mem_start = 0x1f000000;
_appfw_slot_size = 0x80000;
_boot2_sram0_start = 0x0015C000;

/* ROM-lib .data reservation at the start of SRAM1.  The stock app has its
 * .data at exactly 0x20000000, i.e. 0; CONFIG_ENABLE_ROM_LIBS is off. */
_rom_data = 0;

MEMORY
{
	SRAM0 (rwx)  : ORIGIN = 0x00100000, LENGTH = 384K
	SRAM1 (rwx)  : ORIGIN = 0x20000000, LENGTH = 128K
	NVRAM (rw)   : ORIGIN = 0x480C0000, LENGTH = 4K
	XIP   (rx)   : ORIGIN = 0x1F000000, LENGTH = 512K
}

_main_stack_size = 0x800;
_main_stack_start = ORIGIN(SRAM1) + LENGTH(SRAM1) - _main_stack_size;
_keystore_size = 0x1000;
_keystore_start = ORIGIN(SRAM0) + LENGTH(SRAM0) - _keystore_size;

SECTIONS
{
	.init :
	{
		. = ALIGN(512);
		KEEP(*(.isr_vector))
		KEEP(*(.text.Default_IRQHandler*))
		KEEP(*(.text.HardFault_IRQHandler*))
		/* Code used while XIP is disabled (flash erase/program, QSPI,
		 * clock and PMU switching) must run from RAM. */
		*mw300_flash.o (.text .text.* .rodata .rodata.*)
		*mw300_qspi.o (.text .text.* .rodata .rodata.*)
		*mw300_pmu.o (.text .text.* .rodata .rodata.*)
		*mdev_pm.o (.text .text.* .rodata .rodata.*)
		*mw300_clock.o (.text .text.* .rodata .rodata.*)
		*(.ram .ram.*)
		. = ALIGN(4);
	} > SRAM0

	ASSERT(ADDR(.init) == ORIGIN(SRAM0), "vector table must be at 0x00100000")
	ASSERT(ADDR(.init) + SIZEOF(.init) <= _boot2_sram0_start,
	       ".init overlaps boot2's SRAM0 area")

	/* Large .bss of the Wi-Fi driver goes to SRAM0 to leave SRAM1 room. */
	.bss_sram0 (NOLOAD):
	{
		_bss_sram0 = .;
		*libwifidriver.a:(.bss .bss.* COMMON)
		. = ALIGN(4);
		_ebss_sram0 = .;
	} > SRAM0

	. = ALIGN(4);
	_heap_start = .;
	. = _keystore_start;
	_heap_end = .;
	PROVIDE (end = .);

	.keystore _keystore_start (NOLOAD):
	{
		. = . + _keystore_size;
	} > SRAM0

	/* File offset of .text inside the slot == its offset from 0x1F000000. */
	.text (_flashc_mem_start + _text_offset + SIZEOF(.init)) :
	{
		. = ALIGN(4);
		*(.text.Reset_IRQHandler)
		*(.text .text.* .gnu.linkonce.t.*)
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		. = ALIGN(4);
		*(.eh_frame_hdr)
		*(.eh_frame)
		. = ALIGN(4);
		KEEP(*(.init))
		. = ALIGN(4);
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		. = ALIGN(4);
		__init_array_start = .;
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		__init_array_end = .;
		. = ALIGN(4);
		KEEP(*(.fini))
		. = ALIGN(4);
		__fini_array_start = .;
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		__fini_array_end = .;
		. = ALIGN(8);
		_etext = .;
	}

	ASSERT(ADDR(.text) == _flashc_mem_start + _text_offset + SIZEOF(.init),
	       ".text must start right after .init in the image")

	.rom_data (NOLOAD) :
	{
		_rom_data_start = .;
		. = . + _rom_data;
	} > SRAM1

	ASSERT( _rom_data_start == ORIGIN(SRAM1), "Invalid start address of .rom_data")

	.data :
	{
		_data = .;
		*(.data)
		*(.data.*)
		. = ALIGN(4);
		_edata = .;
	} > SRAM1

	/* The whole image (header + .init + .text + .data) must fit the slot. */
	_image_end_offset = _text_offset + SIZEOF(.init) + SIZEOF(.text) + SIZEOF(.data);
	ASSERT(_image_end_offset <= _appfw_slot_size, "image does not fit the 512 KiB appfw slot")

	.bss_sram1 (NOLOAD):
	{
		_bss_sram1 = .;
		*(.bss)
		*(.bss.*)
		*(COMMON)
		. = ALIGN(4);
		_ebss_sram1 = .;
	} > SRAM1

	ASSERT(_ebss_sram1 < _main_stack_start, "Insufficient space for main stack.")

	_heap_2_start = .;
	. = _main_stack_start;
	_heap_2_end = .;

	._main_stack _main_stack_start (NOLOAD):
	{
		. = ALIGN(4);
		. = . + _main_stack_size;
		. = ALIGN(4);
	} > SRAM1

	_actual_heap_size = _heap_end - _heap_start + _heap_2_end - _heap_2_start;
	ASSERT(_actual_heap_size >= _heap_size, "Insufficient space left for heap.")

	.nvram (NOLOAD):
	{
		/* BootROM uses the first bytes of retention RAM */
		_nvram_start = .;
		. = . + 64;
		. = ALIGN(4);
		_nvram_begin = .;
		*(.nvram)
		*(.nvram.*)
		_nvram_end = .;
		. = ALIGN(4);
		*(.nvram_uninit)
		*(.nvram_uninit.*)
	} > NVRAM

	/DISCARD/ :
	{
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		*(.ARM.extab* .gnu.linkonce.armextab.*)
	}
}
