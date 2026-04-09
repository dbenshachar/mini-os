//
// Created by David Benshachar on 4/9/26.
//

#ifndef MINI_OS_STRINGS_H
#define MINI_OS_STRINGS_H

#include <stddef.h>

int strcmp(const char *s1, const char *s2);
int str_multi_cmp(const char *first, const char *list[], size_t count);

#endif //MINI_OS_STRINGS_H