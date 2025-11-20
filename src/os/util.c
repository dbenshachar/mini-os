#include "util.h"

int str_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == *b;
}

int uimul(uint32_t a, uint32_t b) {
  uint32_t c = a;
  for (int i = 1; i < b; i++) {
    c += a;
  }
  return c;
}
void noop() {};
