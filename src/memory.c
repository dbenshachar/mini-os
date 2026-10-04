#include <stddef.h>
/* Freestanding compiler support (large structure assignments can emit memcpy). */
void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *d = destination; const unsigned char *s = source;
    while (count--) *d++ = *s++;
    return destination;
}
void *memset(void *destination, int value, size_t count) {
    unsigned char *d = destination;
    while (count--) *d++ = (unsigned char)value;
    return destination;
}
