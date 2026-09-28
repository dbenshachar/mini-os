#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void init_malloc();
void *malloc(size_t size);
void free(void *ptr);

#endif
