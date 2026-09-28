#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>

void uart_put(char c);
char uart_get();

int disk_mmio_init();
int disk_read_sector(uint32_t lba, void *buffer);
int disk_write_sector(uint32_t lba, const void *buffer);

#endif
