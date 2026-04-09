#include "mmio.h"

char readchar() {
	return uart_get();
}

void printchar(char c) {
	uart_put(c);
}

void printstr(const char *s) {
	while (*s) {
		printchar(*s++);
	}
}