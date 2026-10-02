/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The guest is linked where the Horizon loader maps the first code set, with
 * text, read-only data and data (with bss) in separate, page-aligned
 * segments, as in a Horizon module.
 */
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x8000000;
	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
	} :text

	. = ALIGN(0x1000);
	.rodata : {
		*(.rodata .rodata.*)
	} :rodata

	. = ALIGN(0x1000);
	.data : {
		*(.data .data.* .got .got.*)
	} :data
	.bss : {
		*(.bss .bss.* COMMON)
	} :data
	. = ALIGN(0x1000);

	/DISCARD/ : {
		*(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr)
		*(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash .gnu.hash)
	}
}
