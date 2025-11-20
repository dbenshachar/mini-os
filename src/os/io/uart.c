#include "uart.h"

void putc(char c) {
    volatile unsigned char *thr = (unsigned char *)UART_THR;
    volatile unsigned char *lsr = (unsigned char *)UART_LSR;

    // Wait for THR empty
    while ((*lsr & UART_LSR_THRE) == 0)
        ;
    *thr = c;
}

void puts(const char *s) {
    while (*s)
        putc(*s++);
}

/// gets a character
int getc(void) {
    volatile unsigned char *rbr = (unsigned char *)UART_RBR;
    volatile unsigned char *lsr = (unsigned char *)UART_LSR;

    // Wait until Data Ready bit is set
    while ((*lsr & UART_LSR_DR) == 0)
        ;
    return *rbr;
}
