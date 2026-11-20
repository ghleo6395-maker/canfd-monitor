/* STM32H743VI: 2 MiB internal flash, 512 KiB AXI SRAM, 128 KiB DTCM. */
ENTRY(Reset_Handler)
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 2048K
         RAM (xrw) : ORIGIN = 0x24000000, LENGTH = 512K
         D2_RAM (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
         D3_RAM (xrw) : ORIGIN = 0x38000000, LENGTH = 64K
         DTCM_CPU (xrw) : ORIGIN = 0x20000000, LENGTH = 112K
         DTCM_STACK (xrw) : ORIGIN = 0x2001C000, LENGTH = 16K }
_estack = ORIGIN(DTCM_STACK) + LENGTH(DTCM_STACK);
SECTIONS {
 .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) } >FLASH
 .text : { . = ALIGN(4); *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); _etext = .; } >FLASH
 .ARM.extab : { *(.ARM.extab*) } >FLASH
 .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH
 .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
 .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
 .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH
 _sidata = LOADADDR(.data);
 .data : { . = ALIGN(32); _sdata = .; *(.data*) . = ALIGN(32); _edata = .; } >RAM AT>FLASH
 .bss (NOLOAD) : { . = ALIGN(32); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(32); _ebss = .; } >RAM
 .revd_d2_bss (NOLOAD) : { . = ALIGN(32); _sd2bss = .; *(.revd_d2_bss*) . = ALIGN(32); _ed2bss = .; } >D2_RAM
 /* CPU-only complete FD display history; distinct from DMA staging. */
 .revd_d3_bss (NOLOAD) : { . = ALIGN(32); _sd3bss = .; *(.revd_d3_bss*) . = ALIGN(32); _ed3bss = .; } >D3_RAM
 /* CPU-only recorder queue plus explicitly placed static RTOS core/SD/USB
  * task stacks. Startup zeroes this entire section; no DMA payload is here. */
 .revd_dtcm_bss (NOLOAD) : { . = ALIGN(32); _sdtcmbss = .; *(.revd_dtcm_bss*) . = ALIGN(32); _edtcmbss = .; } >DTCM_CPU
 .heap (NOLOAD) : { _end = .; end = .; . += 8192; _heap_end = .; } >RAM
 .stack (NOLOAD) : { . = ALIGN(8); _stack_limit = .; . += 16384; } >DTCM_STACK
 /DISCARD/ : { *(.note.GNU-stack) }
}
ASSERT(_heap_end <= ORIGIN(RAM)+LENGTH(RAM), "AXI SRAM overflow")

ASSERT(_ed2bss <= ORIGIN(D2_RAM)+LENGTH(D2_RAM), "D2 SRAM overflow")
ASSERT(_ed3bss <= ORIGIN(D3_RAM)+LENGTH(D3_RAM), "D3 SRAM overflow")

ASSERT(_edtcmbss <= ORIGIN(DTCM_CPU)+LENGTH(DTCM_CPU), "DTCM CPU queue overlaps stack")
