.global _start
_start:
    ldr x30, =stack_top
    mov sp, x30
    bl kmain

    /* PSCI SYSTEM_OFF */
    ldr x0, =0x84000008
    hvc #0

    /* Fallback safety loop */
halt:
    wfe
    b halt