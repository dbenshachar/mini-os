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

#include "script.h"
#define EXEC_SOURCE_LIMIT 65536

int exec(const char *path) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    char *source = sys_malloc(EXEC_SOURCE_LIMIT + 1);
    if (!source) { sys_close(fd); return -1; }
    unsigned long size = 0;
    long n = 0;
    while (size < EXEC_SOURCE_LIMIT &&
           (n = sys_read(fd, source + size, EXEC_SOURCE_LIMIT - size)) > 0)
        size += (unsigned long)n;
    char extra;
    int too_large = size == EXEC_SOURCE_LIMIT && sys_read(fd, &extra, 1) != 0;
    sys_close(fd);
    source[size] = 0;
    int result = n < 0 || too_large ? -1 : script_run(source);
    sys_free(source);
    return result;
}
