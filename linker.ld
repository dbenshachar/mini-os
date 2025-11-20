ENTRY(_start)
SECTIONS{
  . = 0x80000000;                 /* safe load addr for virt */
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss  : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  . += 0x4000;                    /* 16 KiB stack */
  stack_top = .;
}
