#ifndef UART_H
#define UART_H

#include <stdint.h>

#define UART0_BASE 0x10000000UL
#define UART_RBR (UART0_BASE + 0x00)  /// Receiver Buffer Register (read)
#define UART_THR (UART0_BASE + 0x00)  /// Transmit Holding Register (write)
#define UART_LSR (UART0_BASE + 0x05)  /// Line Status Register

#define UART_LSR_DR   0x01  /// Data Ready
#define UART_LSR_THRE 0x20  /// Transmitter Holding Register Empty

void putc(char c);
void puts(const char *s);
int getc(void);

#endif
