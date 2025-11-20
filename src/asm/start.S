    .text
    .global _start
    .align 4
_start:
    la   sp, stack_top       # Load stack pointer address (stack_top must be defined by linker)
    .option push
    .option norelax
    .option pop

    call kmain                # Call kernel main

; halt:
;     j    halt                 # Infinite loop when kmain returns
