#include "io.h"
#include "strings.h"
#include "commands.h"

#define MAX_PARAMS 32
#define MAX_PARAM_LENGTH 16

#define CLEAR_LINES 64

int clear() {
    printchar(0x1B); // Escape character
    printchar('[');
    printchar('2');
    printchar('J');

    printchar(0x1B);
    printchar('[');
    printchar('H');
    return 0;
}

int init_commands() {
    clear();
    return 0;
}

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

int execute(char* cmd) {
    char *params[MAX_PARAMS];
    int param_count = deserialize_params(cmd, params, MAX_PARAMS);
    if (param_count == 0) {return 0;}

    const char *exec_command = params[0];
    if (str_multi_cmp(exec_command, (const char *[]) {"quit", "exit"}, 2)) { return 1; }
    if (strcmp(exec_command, "clear")) { return clear(); }

    printstr("\nInvalid command!");
    return 0;
}