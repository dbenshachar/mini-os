#include <stdint.h>
#include "io.h"

#define MAX_PARAMS 32
#define MAX_PARAM_LENGTH 16

int deserialize_params(char *cmd, char **params, int max_params) {
    int count = 0;
    char *p = cmd;

    while (*p != '\0') {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        if (count >= max_params) break;
        params[count++] = p;

        while (*p != '\0' && *p != ' ') p++;
        if (*p == '\0') break;

        *p = '\0';
        p++;
    }
    return count;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (*s1 == *s2);
}

int execute(char* cmd) {
    char *params[MAX_PARAMS];
    int param_count = deserialize_params(cmd, params, MAX_PARAMS);
    if (param_count == 0) {return 0;}

    const char *exec_command = params[0];
    if (strcmp(exec_command, "quit")) { return 1; }
    printstr("\nInvalid command!");
    return 1;
}