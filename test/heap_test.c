#include "heap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    init_malloc();
    assert(malloc(0) == 0);
    assert(malloc((size_t)-1) == 0);
    void *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc((size_t)i * 13 + 1);
        assert(blocks[i] && (uintptr_t)blocks[i] % 16 == 0);
        ((unsigned char *)blocks[i])[0] = (unsigned char)i;
    }
    for (int i = 0; i < 64; i++) assert(((unsigned char *)blocks[i])[0] == (unsigned char)i);
    /* Free out of order to exercise fragmentation and repeated coalescing. */
    for (int i = 0; i < 64; i += 2) free(blocks[i]);
    for (int i = 1; i < 64; i += 2) free(blocks[i]);
    free(0);
    unsigned char *large = malloc(1024 * 1024 - 32);
    assert(large);
    assert(!malloc(1));
    large[0] = 1;
    large[1024 * 1024 - 33] = 2;
    free(large);
    assert(malloc(1024 * 1024 - 32) == large);
    puts("mini_os heap tests passed");
    return 0;
}
