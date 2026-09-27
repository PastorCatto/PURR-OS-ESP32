/* Boot package for the ESP32-S3. The two windows are the same SRAM seen as code and
 * as data (purr_abi.h, PURR_PKG_S3_*). */
MEMORY
{
    text (RX)  : ORIGIN = 0x40378000, LENGTH = 0x8000
    data (RW)  : ORIGIN = 0x3FC90000, LENGTH = 0x8000
}

ENTRY(purr_pkg_entry)

SECTIONS
{
    .text : { *(.literal .literal.* .text .text.*) } > text
    /* Read-only and initialised data are one block, so it is one copy. */
    .data : { *(.rodata .rodata.* .data .data.*) . = ALIGN(4); } > data
    .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > data
}
