.global _start
_start:
    ldr x30, =stack_top
    mov sp, x30
    bl kmain

    ldr x0, =0x84000008
    hvc #0

halt:
    wfe
    b halt