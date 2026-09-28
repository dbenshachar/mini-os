#include "io.h"
#include "strings.h"
#include "commands.h"
#include "program/syscalls.h"
#include "program/logic.h"

#define MAX_PARAMS 32
#define MAX_PARAM_LENGTH 16
#define MAX_PATH_LENGTH 128

#define CLEAR_LINES 64

static char cwd[MAX_PATH_LENGTH] = "/";

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
    cwd[0] = '/';
    cwd[1] = '\0';
    clear();
    return 0;
}

static unsigned long str_len(const char *s) {
    unsigned long len = 0;
    while (s[len] != '\0') len++;
    return len;
}

static int path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static int path_part_is_dot(const char *part, unsigned long len) {
    return len == 1 && part[0] == '.';
}

static int path_part_is_dotdot(const char *part, unsigned long len) {
    return len == 2 && part[0] == '.' && part[1] == '.';
}

static void pop_path_part(char *path, unsigned long *len) {
    if (*len <= 1) return;
    while (*len > 1 && path[*len - 1] != '/') (*len)--;
    if (*len > 1) (*len)--;
    path[*len] = '\0';
}

static int append_path_part(char *path, unsigned long *len, const char *part, unsigned long part_len) {
    unsigned long i;

    if (*len > 1) {
        if (*len + 1 >= MAX_PATH_LENGTH) return -1;
        path[(*len)++] = '/';
    }

    if (*len + part_len >= MAX_PATH_LENGTH) return -1;
    for (i = 0; i < part_len; i++) path[(*len)++] = part[i];
    path[*len] = '\0';
    return 0;
}

static int normalize_path(const char *input, char *out) {
    char combined[MAX_PATH_LENGTH];
    unsigned long combined_len = 0;
    unsigned long out_len;
    unsigned long i;

    if (input == 0 || input[0] == '\0') input = ".";

    if (path_is_sep(input[0])) {
        combined[combined_len++] = '/';
    } else {
        unsigned long cwd_len = str_len(cwd);
        if (cwd_len >= MAX_PATH_LENGTH) return -1;
        for (i = 0; i < cwd_len; i++) combined[combined_len++] = cwd[i];
        if (combined_len > 1) {
            if (combined_len + 1 >= MAX_PATH_LENGTH) return -1;
            combined[combined_len++] = '/';
        }
    }

    for (i = 0; input[i] != '\0'; i++) {
        if (combined_len + 1 >= MAX_PATH_LENGTH) return -1;
        combined[combined_len++] = input[i];
    }
    combined[combined_len] = '\0';

    out[0] = '/';
    out[1] = '\0';
    out_len = 1;
    i = 0;
    while (combined[i] != '\0') {
        const char *part;
        unsigned long part_len = 0;

        while (path_is_sep(combined[i])) i++;
        if (combined[i] == '\0') break;

        part = &combined[i];
        while (combined[i] != '\0' && !path_is_sep(combined[i])) {
            part_len++;
            i++;
        }

        if (path_part_is_dot(part, part_len)) continue;
        if (path_part_is_dotdot(part, part_len)) {
            pop_path_part(out, &out_len);
            continue;
        }
        if (append_path_part(out, &out_len, part, part_len) != 0) return -1;
    }

    return 0;
}

static void noop_list_entry(const FsDirEntry *entry, void *ctx) {
    (void)entry;
    (void)ctx;
}

static void print_uint(uint32_t value) {
    char digits[10];
    int i = 0;

    if (value == 0) {
        printchar('0');
        return;
    }

    while (value > 0) {
        digits[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) printchar(digits[--i]);
}

static void print_int(int value) {
    if (value < 0) {
        printchar('-');
        print_uint((uint32_t)(-value));
        return;
    }
    print_uint((uint32_t)value);
}

static int join_params(char **params, int start, int count, char *out, unsigned long out_size) {
    int i;
    unsigned long pos = 0;

    for (i = start; i < count; i++) {
        unsigned long j = 0;
        if (i > start) {
            if (pos + 1 >= out_size) return -1;
            out[pos++] = ' ';
        }
        while (params[i][j] != '\0') {
            if (pos + 1 >= out_size) return -1;
            out[pos++] = params[i][j++];
        }
    }

    out[pos] = '\0';
    return 0;
}

static void print_ls_entry(const FsDirEntry *entry, void *ctx) {
    (void)ctx;
    printchar('\n');
    if (entry->is_dir) {
        printstr("[DIR]        ");
        printstr(entry->name);
        printchar('/');
    } else {
        printstr("[FILE]       ");
        printstr(entry->name);
        printstr("    ");
        print_uint(entry->size);
        printstr(" bytes");
    }
}

static void print_command_list() {
    printstr("\nclear - Clears the terminal screen; usage: clear");
    printstr("\ncd - Changes the current directory for relative paths; usage: cd <PATH>");
    printstr("\nmkdir - Creates a new FAT32 directory; usage: mkdir <PATH>");
    printstr("\nrm - Removes a file; usage: rm <PATH>");
    printstr("\nrmdir - Recursively removes a directory and all of its contents; usage: rmdir <PATH>");
    printstr("\npwd - Prints the current working directory; usage: pwd");
    printstr("\ncat/read - Prints a file's contents; usage: cat <PATH>");
    printstr("\nwrite - Creates or replaces a file with text; usage: write <PATH> <TEXT>");
    printstr("\nappend - Adds text to the end of a file; usage: append <PATH> <TEXT>");
    printstr("\ncalc - Evaluates an integer arithmetic expression; usage: calc <EXPR>");
    printstr("\nls - Lists files and directories in a FAT32 directory; usage: ls <PATH>");
    printstr("\nexec - executes file as if each line seperated with a semicolon was ran in the termina; usage: exec <PATH>");
    printstr("\nlscmd - Lists available shell commands with usage; usage: lscmd");
    printstr("\nexit/quit - Shuts down the kernel shell; usage: exit");
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
    if (strcmp(exec_command, "lscmd")) {
        print_command_list();
        return 0;
    }
    if (strcmp(exec_command, "pwd")) {
        printchar('\n');
        printstr(cwd);
        return 0;
    }
    if (strcmp(exec_command, "cd")) {
        char path[MAX_PATH_LENGTH];
        const char *target = param_count >= 2 ? params[1] : "/";
        if (normalize_path(target, path) != 0 || fs_list(path, noop_list_entry, 0) != 0) {
            printstr("\ncd: directory open failed");
            return 0;
        }
        {
            unsigned long i = 0;
            while (path[i] != '\0') {
                cwd[i] = path[i];
                i++;
            }
            cwd[i] = '\0';
        }
        return 0;
    }
    if (strcmp(exec_command, "mkdir")) {
        char path[MAX_PATH_LENGTH];
        if (param_count < 2) {
            printstr("\nusage: mkdir PATH");
            return 0;
        }
        if (normalize_path(params[1], path) != 0) {
            printstr("\nmkdir: path too long");
            return 0;
        }
        if (sys_mkdir(path) != 0) {
            printstr("\nmkdir: create failed");
        }
        return 0;
    }
    if (strcmp(exec_command, "rm")) {
        char path[MAX_PATH_LENGTH];
        if (param_count < 2) {
            printstr("\nusage: rm PATH");
            return 0;
        }
        if (normalize_path(params[1], path) != 0) {
            printstr("\nrm: path too long");
            return 0;
        }
        if (sys_remove(path) != 0) {
            printstr("\nrm: remove failed");
        }
        return 0;
    }
    if (strcmp(exec_command, "rmdir")) {
        char path[MAX_PATH_LENGTH];
        if (param_count < 2) {
            printstr("\nusage: rmdir PATH");
            return 0;
        }
        if (normalize_path(params[1], path) != 0) {
            printstr("\nrmdir: path too long");
            return 0;
        }
        if (sys_rmdir(path) != 0) {
            printstr("\nrmdir: remove failed");
        }
        return 0;
    }
    if (strcmp(exec_command, "ls")) {
        char path[MAX_PATH_LENGTH];
        const char *target = param_count >= 2 ? params[1] : ".";
        if (normalize_path(target, path) != 0 || fs_list(path, print_ls_entry, 0) != 0) {
            printstr("\nls: directory open failed");
        }
        return 0;
    }
    if (strcmp(exec_command, "cat") || strcmp(exec_command, "read")) {
        char buffer[129];
        long n;
        int fd;
        if (param_count < 2) {
            printstr("\nusage: cat PATH");
            return 0;
        }
        {
            char path[MAX_PATH_LENGTH];
            if (normalize_path(params[1], path) != 0) {
                printstr("\ncat: path too long");
                return 0;
            }
            fd = sys_open(path, O_RDONLY);
        }
        if (fd < 0) {
            printstr("\ncat: open failed");
            return 0;
        }
        printchar('\n');
        while ((n = sys_read(fd, buffer, 128)) > 0) {
            long i;
            for (i = 0; i < n; i++) printchar(buffer[i]);
        }
        sys_close(fd);
        if (n < 0) printstr("\ncat: read failed");
        return 0;
    }
    if (strcmp(exec_command, "write") || strcmp(exec_command, "append")) {
        int append = strcmp(exec_command, "append");
        int fd;
        int i;
        char text[256];
        unsigned long tlen = 0;
        int failed = 0;
        if (param_count < 3) {
            printstr(append ? "\nusage: append PATH TEXT" : "\nusage: write PATH TEXT");
            return 0;
        }
        {
            char path[MAX_PATH_LENGTH];
            if (normalize_path(params[1], path) != 0) {
                printstr("\nwrite: path too long");
                return 0;
            }
            fd = sys_open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        }
        if (fd < 0) {
            printstr("\nwrite: open failed");
            return 0;
        }
        for (i = 2; i < param_count; i++) {
            unsigned long len = 0;
            unsigned long k;
            while (params[i][len] != '\0') len++;
            if (i > 2) {
                if (tlen + 1 >= sizeof(text)) { failed = 1; break; }
                text[tlen++] = ' ';
            }
            if (tlen + len >= sizeof(text)) { failed = 1; break; }
            for (k = 0; k < len; k++) text[tlen++] = params[i][k];
        }
        if (!failed && sys_write(fd, text, tlen) != (long)tlen) failed = 1;
        sys_close(fd);
        if (failed) printstr("\nwrite: write failed");
        return 0;
    }
    if (strcmp(exec_command, "calc")) {
        char expression[128];
        int result;

        if (param_count < 2) {
            printstr("\nusage: calc EXPR");
            return 0;
        }
        if (join_params(params, 1, param_count, expression, sizeof(expression)) != 0) {
            printstr("\ncalc: expression too long");
            return 0;
        }
        if (!calc(expression, &result)) {
            printstr("\ncalc: invalid expression");
            return 0;
        }

        printchar('\n');
        print_int(result);
        return 0;
    }
    if (strcmp(exec_command, "tree")) {
        char path[MAX_PATH_LENGTH];
        const char *target = param_count >= 2 ? params[1] : ".";

        
        if (normalize_path(target, path) != 0) {
            printstr("\ntree: path too long");
            return 0;
        }

        if (fs_tree(path) != 0) {
            printstr("\ntree: directory open failed");
        }

        return 0;
    }
    if (strcmp(exec_command, "exec")) {
        if (param_count < 2) {
            printstr("\nexec: path not provided");
            return 0;
        }
        const char *target = params[1];

        char path[MAX_PATH_LENGTH];
        if (normalize_path(target, path) != 0) {
            printstr("\ntree: path too long");
            return 0;
        }

        if (exec(path) != 0) {
            printstr("\nexec: execution failed");
        }

        return 0;
    }

    printstr("\nInvalid command!");
    return 0;
}
