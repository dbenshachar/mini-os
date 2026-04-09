//
// Created by David Benshachar on 4/9/26.
//

#include <stdint.h>

#define UART_BASE 0x09000000
#define UART_DR (*((volatile uint32_t *)(UART_BASE + 0x000)))
#define UART_FR (*((volatile uint32_t *)(UART_BASE + 0x018)))

#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)

void uart_put(char c) {
    while (UART_FR & UART_FR_TXFF);
    UART_DR = c;
}

char uart_get() {
    while (UART_FR & UART_FR_RXFE);
    return UART_DR & 0xFF;
}