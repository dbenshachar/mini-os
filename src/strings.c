//
// Created by David Benshachar on 4/9/26.
//

#include <stddef.h>
#include "strings.h"

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (*s1 == *s2);
}

int str_multi_cmp(const char *first, const char *list[], size_t count) {
    for (size_t i = 0; i < count; i++) {
        const char *s1 = first;
        const char *s2 = list[i];

        while (*s1 && *s2 && *s1 == *s2) {
            s1++;
            s2++;
        }

        if (*s1 == '\0' && *s2 == '\0') {
            return 1;
        }
    }
    return 0;
}