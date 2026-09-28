#include "logic.h"
#include "syscalls.h"
#include "commands.h"

static int expr(const char **s, int *err);

static void skip(const char **s) {
    while (**s == ' ' || **s == '\t') (*s)++;
}

static int factor(const char **s, int *err) {
    skip(s);
    if (**s == '-') { (*s)++; return -factor(s, err); }
    if (**s == '+') { (*s)++; return factor(s, err); }
    if (**s == '(') {
        (*s)++;
        int v = expr(s, err);
        skip(s);
        if (**s != ')') { *err = 1; return 0; }
        (*s)++;
        return v;
    }
    if (**s < '0' || **s > '9') { *err = 1; return 0; }
    int v = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    if (**s == '.') *err = 1;
    return v;
}

static int term(const char **s, int *err) {
    int v = factor(s, err);
    for (;;) {
        skip(s);
        if (**s == '*') { (*s)++; v *= factor(s, err); }
        else if (**s == '/') {
            (*s)++;
            int d = factor(s, err);
            if (d == 0) { *err = 1; return 0; }
            v /= d;
        } else return v;
    }
}

static int expr(const char **s, int *err) {
    int v = term(s, err);
    for (;;) {
        skip(s);
        if (**s == '+') { (*s)++; v += term(s, err); }
        else if (**s == '-') { (*s)++; v -= term(s, err); }
        else return v;
    }
}

int calc(const char* expression, int* out) {
    int err = 0;
    const char *s = expression;
    int v = expr(&s, &err);
    skip(&s);
    if (err || *s != '\0') return 0;
    *out = v;
    return 1;
}

#define EXEC_BUF_SIZE 1024

int exec(const char* path) {
    int fd = fs_open(path, 0);
    if (fd < 0) return -1;

    char cmd[EXEC_BUF_SIZE];
    char tmp[EXEC_BUF_SIZE];
    char chunk[128];
    int len = 0;
    long n;
    int pending_while = 0;

    while ((n = fs_read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];

            if (c == ';') {
                cmd[len] = '\0';
                if (pending_while > 0) {
                    for (int r = 0; r < pending_while - 1; r++) {
                        for (int j = 0; j <= len; j++) tmp[j] = cmd[j];
                        execute(tmp);
                    }
                    pending_while = 0;
                } else if (len > 6 && cmd[0] == 'w' && cmd[1] == 'h' && cmd[2] == 'i' && cmd[3] == 'l' && cmd[4] == 'e' && cmd[5] == ' ') {
                    int k = 6;
                    int v = 0;
                    while (cmd[k] >= '0' && cmd[k] <= '9') {
                        v = v * 10 + (cmd[k] - '0');
                        k++;
                    }
                    if (k > 6 && cmd[k] == '\0') {
                        pending_while = v;
                        len = 0;
                        continue;
                    }
                }
                execute(cmd);
                len = 0;
            } else if (len < EXEC_BUF_SIZE - 1) {
                cmd[len++] = c;
            }
        }
    }

    fs_close(fd);
    return 0;
}
