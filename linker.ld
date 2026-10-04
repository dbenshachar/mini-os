ENTRY(_start)
SECTIONS{
  . = 0x40100000;                 /* safe load addr for virt */
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss  : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  . += 0x20000;                   /* 128 KiB stack for bounded interpreter calls */
  stack_top = .;
}