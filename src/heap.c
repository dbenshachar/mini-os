#include "heap.h"
#include <stdint.h>

/* Scripts and network streams need more than the original 10 KB pool. */
#define HEAP_SIZE (1024 * 1024)
#define ALIGNMENT 16u
static unsigned char heap_pool[HEAP_SIZE] __attribute__((aligned(ALIGNMENT)));
typedef struct Block {
    size_t size;
    int available;
    struct Block *next;
} Block;
#define HEADER_SIZE ((sizeof(Block) + ALIGNMENT - 1) & ~(ALIGNMENT - 1))
static Block *free_list;

void init_malloc(void) {
    free_list = (Block *)heap_pool;
    free_list->size = HEAP_SIZE - HEADER_SIZE;
    free_list->available = 1;
    free_list->next = 0;
}

void *malloc(size_t size) {
    if (!size || size > HEAP_SIZE - HEADER_SIZE) return 0;
    size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    for (Block *b = free_list; b; b = b->next) {
        if (!b->available || b->size < size) continue;
        if (b->size >= size + HEADER_SIZE + ALIGNMENT) {
            Block *tail = (Block *)((unsigned char *)b + HEADER_SIZE + size);
            tail->size = b->size - size - HEADER_SIZE;
            tail->available = 1;
            tail->next = b->next;
            b->next = tail;
            b->size = size;
        }
        b->available = 0;
        return (unsigned char *)b + HEADER_SIZE;
    }
    return 0;
}

void free(void *ptr) {
    if (!ptr) return;
    /* Only accept the start of a live allocation, never an arbitrary pointer. */
    for (Block *b = free_list; b; b = b->next) {
        if ((unsigned char *)b + HEADER_SIZE == ptr) {
            b->available = 1;
            break;
        }
    }
    for (Block *b = free_list; b && b->next;) {
        if (b->available && b->next->available) {
            b->size += HEADER_SIZE + b->next->size;
            b->next = b->next->next;
        } else b = b->next;
    }
}
