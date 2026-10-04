#include "script.h"
#include "syscalls.h"
#include "commands.h"
#include "io.h"
#include "net.h"
#include <stdint.h>

/* A bounded text interpreter: bodies are retained as source and re-expanded at
 * each call. No bytecode, AST, or native code generation is involved. */
#define NAME_MAX 48
#define VAR_MAX 256
#define FUNC_MAX 48
#define ARG_MAX 16
#define OBJECT_MAX 4096
#define DATA_MAX 65536
#define STEP_MAX 100000
#define DEPTH_MAX 16

enum { NUMBER, STRING, BUFFER, ARRAY };
typedef struct Object Object;
typedef struct { int type; int64_t number; Object *object; } Value;
struct Object { Object *next; void *data; size_t length; int type; const char *literal; };
typedef struct { char name[NAME_MAX]; Value value; } Variable;
typedef struct {
    char name[NAME_MAX], params[ARG_MAX][NAME_MAX];
    int count;
    const char *body, *end;
} Function;
typedef struct {
    Variable vars[VAR_MAX]; Function funcs[FUNC_MAX];
    int vars_count, funcs_count, floor, depth, steps, objects_count, expression_depth, call_depth;
    int files[16], sockets[4];
    Object *objects;
    const char *error;
    const char *source;
    const char *error_at;
    int stopped;
} Context;
enum { END = 256, IDENT, INTEGER, TEXT, EQ, NE, LE, GE, AND, OR };
typedef struct { int kind; const char *start; size_t size; int64_t number; } Token;
typedef struct { Context *c; const char *p, *end; Token t; } Parser;
typedef struct { const char *start, *end; } Body;

static Value number(int64_t n) { Value v = { NUMBER, n, 0 }; return v; }
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int private_name(const char *s) {
    const char *prefix = "__script_";
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}
static size_t length(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void copy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
}
static int alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int digit(char ch) { return ch >= '0' && ch <= '9'; }
static void fail(Parser *p, const char *message) {
    if (!p->c->error) { p->c->error = message; p->c->error_at = p->t.start; }
}
static int tick(Parser *p) {
    if (++p->c->steps > STEP_MAX) fail(p, "execution limit exceeded");
    return !p->c->error && !p->c->stopped;
}
static void next(Parser *p) {
    const char *s = p->p;
    for (;;) {
        while (s < p->end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
        if (s < p->end && (*s == '#' || (*s == '/' && s + 1 < p->end && s[1] == '/'))) {
            while (s < p->end && *s != '\n') s++;
        } else break;
    }
    p->t.start = s; p->t.size = 0; p->t.number = 0;
    if (s >= p->end || !*s) { p->t.kind = END; p->p = s; return; }
    char ch = *s++;
    if (alpha(ch) || ch == '$') {
        if (ch == '$') p->t.start = s;
        while (s < p->end && (alpha(*s) || digit(*s))) s++;
        p->t.kind = IDENT;
    } else if (digit(ch)) {
        uint64_t n = (unsigned)(ch - '0');
        while (s < p->end && digit(*s)) {
            unsigned d = (unsigned)(*s++ - '0');
            if (n > (UINT64_MAX - d) / 10) fail(p, "integer literal overflow");
            n = n * 10 + d;
        }
        p->t.kind = INTEGER; p->t.number = (int64_t)n;
    } else if (ch == '"' || ch == '\'') {
        p->t.kind = TEXT; p->t.start = s;
        while (s < p->end && *s != ch) {
            if (*s == '\\' && s + 1 < p->end) s++;
            s++;
        }
        p->t.size = (size_t)(s - p->t.start);
        if (s == p->end) fail(p, "unterminated string"); else s++;
        p->p = s; return;
    } else {
        p->t.kind = ch;
        if (s < p->end) {
            int k = 0;
            if (ch == '=' && *s == '=') k = EQ;
            if (ch == '!' && *s == '=') k = NE;
            if (ch == '<' && *s == '=') k = LE;
            if (ch == '>' && *s == '=') k = GE;
            if (ch == '&' && *s == '&') k = AND;
            if (ch == '|' && *s == '|') k = OR;
            if (k) { p->t.kind = k; s++; }
        }
    }
    p->t.size = (size_t)(s - p->t.start); p->p = s;
}
static Parser parser(Context *c, const char *start, const char *end) {
    Parser p; p.c = c; p.p = start; p.end = end; next(&p); return p;
}
static int is(Parser *p, const char *word) {
    size_t n = length(word);
    if (p->t.kind != IDENT || p->t.size != n) return 0;
    for (size_t i = 0; i < n; i++) if (p->t.start[i] != word[i]) return 0;
    return 1;
}
static void name(Parser *p, char *out) {
    if (p->t.kind != IDENT || !p->t.size || p->t.size >= NAME_MAX) {
        fail(p, "invalid or overlong identifier"); out[0] = 0; return;
    }
    copy(out, p->t.start, p->t.size); out[p->t.size] = 0;
    /* Expanded identifiers are allowed inside a call; user declarations using
     * this prefix are rejected separately in the declaration parser. */
    next(p);
}
static void expect(Parser *p, int kind) {
    if (p->t.kind != kind) fail(p, "unexpected token"); else next(p);
}
static int find_var(Context *c, const char *id) {
    for (int i = c->vars_count - 1; i >= 0; i--) if (equal(c->vars[i].name, id)) return i;
    return -1;
}
static int bind(Parser *p, const char *id, Value v, int declare) {
    Context *c = p->c;
    int i = find_var(c, id);
    if (declare && i >= c->floor) { fail(p, "variable already declared"); return -1; }
    if (declare || i < 0) {
        if (c->vars_count == VAR_MAX) { fail(p, "too many variables"); return -1; }
        i = c->vars_count++;
        copy(c->vars[i].name, id, length(id) + 1);
    }
    c->vars[i].value = v; return i;
}
static Object *live(Parser *p, Value v) {
    if (v.type == NUMBER || !v.object || !v.object->data) {
        fail(p, "expected live string, array, or buffer"); return 0;
    }
    return v.object;
}
static Value object(Parser *p, int type, size_t n) {
    Value v = { type, 0, 0 };
    if (n > (type == ARRAY ? 4096u : DATA_MAX) || p->c->objects_count == OBJECT_MAX) {
        fail(p, "allocation limit exceeded"); return v;
    }
    Object *o = sys_malloc(sizeof(*o));
    if (!o) { fail(p, "out of memory"); return v; }
    size_t bytes = type == ARRAY ? n * sizeof(Value) : n + 1;
    o->data = sys_malloc(bytes ? bytes : 1);
    if (!o->data) { sys_free(o); fail(p, "out of memory"); return v; }
    o->type = type; o->literal = 0; o->length = n; o->next = p->c->objects;
    p->c->objects = o; p->c->objects_count++;
    if (type == ARRAY) for (size_t i = 0; i < n; i++) ((Value *)o->data)[i] = number(0);
    else for (size_t i = 0; i <= n; i++) ((char *)o->data)[i] = 0;
    v.object = o; return v;
}
static int64_t numeric(Parser *p, Value v) {
    if (v.type != NUMBER) { fail(p, "expected a number"); return 0; }
    return v.number;
}
static int truth(Parser *p, Value v) { return numeric(p, v) != 0; }
static size_t count(Parser *p, Value v) {
    int64_t n = numeric(p, v);
    if (n < 0 || n > DATA_MAX) { fail(p, "invalid size or index"); return 0; }
    return (size_t)n;
}
static char *text(Parser *p, Value v) {
    Object *o = live(p, v);
    if (!o) return 0;
    if (v.type != STRING) { fail(p, "expected a string"); return 0; }
    /* Paths and IP addresses must not contain embedded NUL bytes. */
    for (size_t i = 0; i < o->length; i++) if (!((char *)o->data)[i]) {
        fail(p, "embedded NUL in text argument"); return 0;
    }
    return o->data;
}
static size_t decimal(char *out, int64_t n) {
    char digits[20]; int k = 0; size_t pos = 0;
    uint64_t u = n < 0 ? 0u - (uint64_t)n : (uint64_t)n;
    if (n < 0) out[pos++] = '-';
    do { digits[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) out[pos++] = digits[--k];
    out[pos] = 0; return pos;
}
static Value expression(Parser *p, int precedence, int run);
static int statements(Parser *p, Value *returned);
static int statement(Parser *p, Value *returned);

/* Replace parameter identifiers in stored source with private frame names.
 * Quoted strings and comments are copied verbatim; replacing tokens prevents
 * accidental changes to longer names (e.g. x must not alter next). */
static char *expand_function(Parser *p, Function *f, char aliases[ARG_MAX][NAME_MAX]) {
    char *out = 0;
    size_t pos = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *s = f->body;
        pos = 0;
        int quote = 0, comment = 0;
        while (s < f->end && !p->c->error) {
            const char *chunk = s;
            size_t n = 1;
            if (comment) { if (*s == '\n') comment = 0; }
            else if (quote) {
                if (*s == '\\' && s + 1 < f->end) n = 2;
                else if (*s == quote) quote = 0;
            } else if (*s == '#' || (*s == '/' && s + 1 < f->end && s[1] == '/')) comment = 1;
            else if (*s == '"' || *s == '\'') quote = *s;
            else if (alpha(*s)) {
                const char *end = s + 1;
                while (end < f->end && (alpha(*end) || digit(*end))) end++;
                n = (size_t)(end - s);
                for (int i = 0; i < f->count; i++) {
                    if (length(f->params[i]) != n) continue;
                    size_t j = 0;
                    while (j < n && s[j] == f->params[i][j]) j++;
                    if (j == n) { chunk = aliases[i]; break; }
                }
            }
            size_t size = chunk == s ? n : length(chunk);
            if (pos + size > DATA_MAX) { fail(p, "function expansion too long"); break; }
            if (out) copy(out + pos, chunk, size);
            pos += size; s += n;
        }
        if (p->c->error) { sys_free(out); return 0; }
        if (!pass) {
            out = sys_malloc(pos + 1);
            if (!out) { fail(p, "out of memory"); return 0; }
        }
    }
    out[pos] = 0;
    return out;
}
/* Bodies are expanded and parsed at every call; argument values are substituted
 * when their private identifiers are evaluated, including shared object handles. */
static Value call(Parser *p, const char *id, Value *args, int argc) {
    Context *c = p->c;
    for (int i = 0; i < c->funcs_count; i++) {
        Function *f = &c->funcs[i];
        if (!equal(f->name, id)) continue;
        if (argc != f->count) { fail(p, "wrong function argument count"); return number(0); }
        if (c->depth >= DEPTH_MAX) { fail(p, "call depth exceeded"); return number(0); }
        int saved = c->vars_count, floor = c->floor;
        c->floor = saved; c->depth++; c->call_depth++;
        char aliases[ARG_MAX][NAME_MAX];
        for (int j = 0; j < argc; j++) {
            copy(aliases[j], "__script_arg_", 13);
            size_t pos = 13;
            pos += decimal(aliases[j] + pos, c->call_depth);
            aliases[j][pos++] = '_';
            decimal(aliases[j] + pos, j);
            bind(p, aliases[j], args[j], 1);
        }
        char *expanded = expand_function(p, f, aliases);
        Value result = number(0);
        if (expanded) {
            size_t n = length(expanded);
            Parser body = parser(c, expanded, expanded + n);
            statements(&body, &result);
            /* Literals remain owned by the script, but their temporary source
             * addresses cannot serve as cache keys after the expansion is freed. */
            for (Object *o = c->objects; o; o = o->next)
                if ((uintptr_t)o->literal >= (uintptr_t)expanded &&
                    (uintptr_t)o->literal <= (uintptr_t)(expanded + n)) o->literal = 0;
            if (c->error) c->error_at = p->t.start;
            sys_free(expanded);
        }
        c->vars_count = saved; c->floor = floor; c->depth--; c->call_depth--;
        return result;
    }
#define BUILTIN(n, arity) if (equal(id, n)) { if (argc != arity) { fail(p, "wrong builtin argument count"); return number(0); }
#define DONE }
    BUILTIN("malloc", 1)
        size_t n = count(p, args[0]);
        return c->error ? number(0) : object(p, BUFFER, n);
    DONE
    BUILTIN("array", 1)
        size_t n = count(p, args[0]);
        return c->error ? number(0) : object(p, ARRAY, n);
    DONE
    BUILTIN("free", 1)
        if (args[0].type == NUMBER && args[0].number == 0) return number(0);
        Object *o = live(p, args[0]);
        if (o) { sys_free(o->data); o->data = 0; }
        return number(0);
    DONE
    BUILTIN("len", 1)
        Object *o = live(p, args[0]); return number(o ? (int64_t)o->length : 0);
    DONE
    BUILTIN("str", 1)
        char buf[22]; size_t n = decimal(buf, numeric(p, args[0]));
        if (c->error) return number(0);
        Value v = object(p, STRING, n);
        if (v.object) copy(v.object->data, buf, n);
        return v;
    DONE
    BUILTIN("chr", 1)
        int64_t n = numeric(p, args[0]);
        if (n < 0 || n > 255) fail(p, "byte value out of range");
        if (c->error) return number(0);
        Value v = object(p, STRING, 1);
        if (v.object) ((unsigned char *)v.object->data)[0] = (unsigned char)n;
        return v;
    DONE
    if (equal(id, "print")) {
        for (int i = 0; i < argc; i++) {
            if (args[i].type == NUMBER) { char b[22]; decimal(b, args[i].number); printstr(b); }
            else {
                Object *o = live(p, args[i]);
                if (o && args[i].type != ARRAY) for (size_t j = 0; j < o->length; j++) printchar(((char *)o->data)[j]);
                else if (o) fail(p, "print expects a number, string, or buffer");
            }
        }
        printchar('\n'); return number(0);
    }
    BUILTIN("open", 2)
        char *path = text(p, args[0]); int64_t flags = numeric(p, args[1]);
        if (c->error) return number(-1);
        int fd = sys_open(path, (int)flags);
        if (fd >= 0 && fd < 16) c->files[fd] = 1;
        return number(fd);
    DONE
    BUILTIN("close", 1)
        int64_t fd = numeric(p, args[0]);
        if (c->error || fd < 0 || fd >= 16 || !c->files[fd]) return number(-1);
        int result = sys_close((int)fd); c->files[fd] = 0; return number(result);
    DONE
    BUILTIN("tcp_connect", 2)
        char *ip = text(p, args[0]); int64_t port = numeric(p, args[1]);
        if (c->error || port < 1 || port > 65535) return number(-1);
        int fd = tcp_connect(ip, (unsigned short)port);
        if (fd >= 0 && fd < 4) c->sockets[fd] = 1;
        return number(fd);
    DONE
    BUILTIN("tcp_close", 1)
        int64_t fd = numeric(p, args[0]);
        if (c->error || fd < 0 || fd >= 4 || !c->sockets[fd]) return number(-1);
        int result = tcp_close((int)fd); c->sockets[fd] = 0; return number(result);
    DONE
    if (equal(id, "read") || equal(id, "write") || equal(id, "tcp_send") || equal(id, "tcp_recv")) {
        if (argc != 3) { fail(p, "stream operation needs descriptor, buffer, count"); return number(-1); }
        int64_t fd = numeric(p, args[0]); Object *o = live(p, args[1]); size_t n = count(p, args[2]);
        int receiving = equal(id, "read") || equal(id, "tcp_recv");
        int networking = equal(id, "tcp_send") || equal(id, "tcp_recv");
        if (!o || args[1].type == ARRAY || n > (o ? o->length : 0) || (receiving && args[1].type != BUFFER))
            fail(p, "invalid stream buffer or size");
        if (c->error) return number(-1);
        if (fd < 0 || fd >= (networking ? 4 : 16) || !(networking ? c->sockets[fd] : c->files[fd])) return number(-1);
        long result;
        if (networking) result = receiving ? tcp_recv((int)fd, o->data, n) : tcp_send((int)fd, o->data, n);
        else result = receiving ? sys_read((int)fd, o->data, n) : sys_write((int)fd, o->data, n);
        return number(result);
    }
    BUILTIN("slice", 3)
        Object *o = live(p, args[0]); size_t start = count(p, args[1]), n = count(p, args[2]);
        if (!o || args[0].type == ARRAY || start > o->length || n > o->length - start) fail(p, "slice out of bounds");
        if (c->error) return number(0);
        Value v = object(p, STRING, n); if (v.object) copy(v.object->data, (char *)o->data + start, n); return v;
    DONE
    if (equal(id, "mkdir") || equal(id, "remove") || equal(id, "rmdir")) {
        if (argc != 1) { fail(p, "wrong builtin argument count"); return number(-1); }
        char *path = text(p, args[0]); if (c->error) return number(-1);
        return number(equal(id, "mkdir") ? sys_mkdir(path) : equal(id, "remove") ? sys_remove(path) : sys_rmdir(path));
    }
    fail(p, "unknown function"); return number(0);
#undef BUILTIN
#undef DONE
}

static Value indexed(Parser *p, Value v, Value index) {
    Object *o = live(p, v); size_t i = count(p, index);
    if (!o || i >= o->length) { fail(p, "index out of bounds"); return number(0); }
    return v.type == ARRAY ? ((Value *)o->data)[i] : number(((unsigned char *)o->data)[i]);
}
static Value primary(Parser *p, int run) {
    Value v = number(0);
    if (p->t.kind == INTEGER) { v = number(p->t.number); next(p); }
    else if (p->t.kind == TEXT) {
        Token t = p->t; next(p);
        if (run) {
            for (Object *o = p->c->objects; o; o = o->next) {
                if (o->data && o->literal == t.start) { v.type = STRING; v.object = o; break; }
            }
            if (v.object) goto postfix;
            v = object(p, STRING, t.size);
            if (v.object) {
                v.object->literal = t.start;
                size_t n = 0;
                for (size_t i = 0; i < t.size; i++) {
                    char ch = t.start[i];
                    if (ch == '\\' && i + 1 < t.size) {
                        ch = t.start[++i];
                        if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r';
                        else if (ch == 't') ch = '\t'; else if (ch == '0') ch = 0;
                        else if (ch != '\\' && ch != '"' && ch != '\'') fail(p, "unknown string escape");
                    }
                    ((char *)v.object->data)[n++] = ch;
                }
                v.object->length = n; ((char *)v.object->data)[n] = 0;
            }
        }
    } else if (p->t.kind == IDENT) {
        char id[NAME_MAX]; name(p, id);
        if (p->t.kind == '(') {
            next(p); Value args[ARG_MAX]; int argc = 0;
            if (p->t.kind != ')') for (;;) {
                if (argc == ARG_MAX) { fail(p, "too many arguments"); break; }
                args[argc++] = expression(p, 1, run);
                if (p->t.kind != ',') break;
                next(p);
            }
            expect(p, ')'); if (run && !p->c->error && tick(p)) v = call(p, id, args, argc);
        } else if (run) {
            int i = find_var(p->c, id);
            if (i < 0) fail(p, "undefined variable"); else v = p->c->vars[i].value;
        }
    } else if (p->t.kind == '(') {
        next(p); v = expression(p, 1, run); expect(p, ')');
    } else if (p->t.kind == '[') {
        next(p); Value *elements = run ? sys_malloc(256 * sizeof(Value)) : 0; int n = 0;
        if (run && !elements) fail(p, "out of memory");
        if (p->t.kind != ']') for (;;) {
            if (n == 256) { fail(p, "array literal too large (use array)"); break; }
            Value element = expression(p, 1, run);
            if (elements) elements[n] = element;
            n++;
            if (p->t.kind != ',') break;
            next(p);
        }
        expect(p, ']');
        if (run && !p->c->error) { v = object(p, ARRAY, (size_t)n); if (v.object) copy(v.object->data, elements, (size_t)n * sizeof(Value)); }
        sys_free(elements);
    } else if (p->t.kind == '-' || p->t.kind == '+' || p->t.kind == '!') {
        int op = p->t.kind; next(p); v = expression(p, 8, run);
        if (run) { int64_t n = numeric(p, v); v = number(op == '-' ? (int64_t)(0u - (uint64_t)n) : op == '!' ? !n : n); }
    } else { fail(p, "expected expression"); if (p->t.kind != END) next(p); }
postfix:
    while (p->t.kind == '[' && !p->c->error) {
        next(p); Value i = expression(p, 1, run); expect(p, ']');
        if (run) v = indexed(p, v, i);
    }
    return v;
}
static int precedence(int op) {
    switch (op) {
        case OR: return 1; case AND: return 2; case EQ: case NE: return 3;
        case '<': case '>': case LE: case GE: return 4;
        case '+': case '-': return 5; case '*': case '/': case '%': return 6;
        default: return 0;
    }
}
static Value binary(Parser *p, int op, Value a, Value b) {
    if ((op == EQ || op == NE) && (a.type != NUMBER || b.type != NUMBER)) {
        Object *x = a.type != NUMBER ? live(p, a) : 0;
        Object *y = b.type != NUMBER ? live(p, b) : 0;
        int eq = a.type == b.type && x && y && x == y;
        if (a.type == STRING && b.type == STRING && x && y) {
            eq = x->length == y->length;
            for (size_t i = 0; eq && i < x->length; i++) if (((char *)x->data)[i] != ((char *)y->data)[i]) eq = 0;
        }
        return number(op == EQ ? eq : !eq);
    }
    if (op == '+' && a.type == STRING && b.type == STRING) {
        Object *x = live(p, a), *y = live(p, b);
        if (!x || !y) return number(0);
        Value v = object(p, STRING, x->length + y->length);
        if (v.object) { copy(v.object->data, x->data, x->length); copy((char *)v.object->data + x->length, y->data, y->length); }
        return v;
    }
    int64_t x = numeric(p, a), y = numeric(p, b);
    switch (op) {
        case '+': return number((int64_t)((uint64_t)x + (uint64_t)y));
        case '-': return number((int64_t)((uint64_t)x - (uint64_t)y));
        case '*': return number((int64_t)((uint64_t)x * (uint64_t)y));
        case '/': case '%':
            if (!y) { fail(p, "division by zero"); return number(0); }
            if (x == INT64_MIN && y == -1) return number(op == '/' ? INT64_MIN : 0);
            return number(op == '/' ? x / y : x % y);
        case EQ: return number(x == y); case NE: return number(x != y);
        case '<': return number(x < y); case '>': return number(x > y);
        case LE: return number(x <= y); case GE: return number(x >= y);
        case AND: return number(x && y); case OR: return number(x || y);
        default: return number(0);
    }
}
static Value expression(Parser *p, int min, int run) {
    if (++p->c->expression_depth > 32) {
        fail(p, "expression depth exceeded"); p->c->expression_depth--; return number(0);
    }
    Value v = primary(p, run);
    while (!p->c->error && precedence(p->t.kind) >= min) {
        int op = p->t.kind, prec = precedence(op); next(p);
        int rhs_run = run;
        if (run && (op == AND || op == OR)) {
            int t = truth(p, v); if ((op == AND && !t) || (op == OR && t)) rhs_run = 0;
        }
        Value rhs = expression(p, prec + 1, rhs_run);
        if (run && !p->c->error) {
            if (!rhs_run && (op == AND || op == OR)) v = number(op == OR);
            else v = binary(p, op, v, rhs);
        }
    }
    p->c->expression_depth--;
    return v;
}
static Body block(Parser *p) {
    Body b = {p->p, p->p};
    if (p->t.kind != '{') { fail(p, "expected block in braces"); return b; }
    b.start = p->p; int depth = 1; next(p);
    while (depth && p->t.kind != END && !p->c->error) {
        if (p->t.kind == '{') depth++;
        if (p->t.kind == '}') { depth--; if (!depth) b.end = p->t.start; }
        next(p);
    }
    if (depth) fail(p, "unterminated block");
    return b;
}
static int run_body(Parser *p, Body b, Value *ret) {
    if (p->c->depth >= DEPTH_MAX) { fail(p, "block depth exceeded"); return 0; }
    p->c->depth++;
    int saved = p->c->vars_count, floor = p->c->floor;
    p->c->floor = saved;
    Parser body = parser(p->c, b.start, b.end);
    int returned = statements(&body, ret);
    p->c->vars_count = saved; p->c->floor = floor;
    p->c->depth--; return returned;
}
static void end_statement(Parser *p) {
    if (p->t.kind == ';') next(p);
    else if (p->t.kind != END) fail(p, "expected semicolon");
}
/* Shell commands still run through execute(), with identifier/$name substitution
 * outside quoted strings. Expansion is bounded and never reparsed as script. */
static void shell(Parser *p) {
    const char *start = p->t.start, *end = start;
    int quote = 0;
    while (end < p->end) {
        char ch = *end;
        if (quote) {
            if (ch == '\\' && end + 1 < p->end) { end += 2; continue; }
            if (ch == quote) quote = 0;
        } else {
            if (ch == '"' || ch == '\'') quote = ch;
            if (ch == ';' || ch == '}') break;
        }
        end++;
    }
    if (quote) { fail(p, "unterminated shell quote"); return; }
    char *cmd = sys_malloc(DATA_MAX + 1);
    if (!cmd) { fail(p, "out of memory"); return; }
    size_t pos = 0; const char *s = start; quote = 0;
    while (s < end && !p->c->error) {
        if (!quote && (alpha(*s) || *s == '$') && s != start) {
            const char *id = s; if (*id == '$') id++;
            const char *stop = id; while (stop < end && (alpha(*stop) || digit(*stop))) stop++;
            char n[NAME_MAX]; size_t len = (size_t)(stop - id);
            int i = -1;
            if (len && len < NAME_MAX) { copy(n, id, len); n[len] = 0; i = find_var(p->c, n); }
            if (i >= 0) {
                Value v = p->c->vars[i].value; char digits[22]; const char *data; size_t size;
                if (v.type == NUMBER) { size = decimal(digits, v.number); data = digits; }
                else {
                    Object *o = live(p, v);
                    if (!o || v.type != STRING) { fail(p, "shell substitution needs number or string"); break; }
                    data = o->data; size = o->length;
                }
                if (pos + size > DATA_MAX) { fail(p, "shell expansion too long"); break; }
                copy(cmd + pos, data, size); pos += size; s = stop; continue;
            }
            if (*s == '$') { fail(p, "undefined shell variable"); break; }
        }
        if (pos == DATA_MAX) { fail(p, "shell command too long"); break; }
        char ch = *s++;
        if (ch == '"' || ch == '\'') { if (!quote) quote = ch; else if (quote == ch) quote = 0; }
        cmd[pos++] = ch;
    }
    cmd[pos] = 0;
    if (!p->c->error && execute(cmd)) p->c->stopped = 1;
    sys_free(cmd); p->p = end < p->end && *end == ';' ? end + 1 : end; next(p);
}
static int statement(Parser *p, Value *ret) {
    if (!tick(p)) return 0;
    if (p->t.kind == ';') { next(p); return 0; }
    if (is(p, "fn") || is(p, "function") || is(p, "def")) {
        next(p);
        if (p->c->depth) { fail(p, "functions must be defined at top level"); return 0; }
        if (p->c->funcs_count == FUNC_MAX) { fail(p, "too many functions"); return 0; }
        Function f; f.count = 0; name(p, f.name); expect(p, '(');
        if (p->t.kind != ')') for (;;) {
            if (f.count == ARG_MAX) { fail(p, "too many function parameters"); break; }
            name(p, f.params[f.count]);
            if (private_name(f.params[f.count])) fail(p, "reserved identifier prefix");
            for (int i = 0; i < f.count; i++) if (equal(f.params[i], f.params[f.count])) fail(p, "duplicate parameter");
            f.count++;
            if (p->t.kind != ',') break;
            next(p);
        }
        expect(p, ')'); Body b = block(p); f.body = b.start; f.end = b.end;
        for (int i = 0; i < p->c->funcs_count; i++) if (equal(f.name, p->c->funcs[i].name)) fail(p, "function already defined");
        if (!p->c->error) p->c->funcs[p->c->funcs_count++] = f;
        return 0;
    }
    if (is(p, "if")) {
        next(p); Value cond = expression(p, 1, 1); int yes = truth(p, cond);
        Body then = block(p), other = {p->p, p->p}; int has_else = 0;
        if (is(p, "else")) { next(p); other = block(p); has_else = 1; }
        if (!p->c->error && (yes || has_else)) return run_body(p, yes ? then : other, ret);
        return 0;
    }
    if (is(p, "while")) {
        next(p); const char *condition = p->t.start;
        Value cond = expression(p, 1, 1); const char *condition_end = p->t.start;
        if (p->t.kind == ';') {
            /* Original syntax: while N; command; repeats the following command N times. */
            int64_t times = numeric(p, cond); if (times < 0 || times > STEP_MAX) fail(p, "invalid repeat count");
            next(p); const char *command = p->t.start;
            Parser scan = *p;
            while (scan.t.kind != END && scan.t.kind != ';' && !scan.c->error) next(&scan);
            const char *stop = scan.t.start;
            if (scan.t.kind == ';') next(&scan);
            *p = scan;
            for (int64_t i = 0; i < times && !p->c->error; i++) {
                Parser body = parser(p->c, command, stop);
                if (statement(&body, ret)) return 1;
            }
            return 0;
        }
        Body b = block(p);
        while (!p->c->error && !p->c->stopped && truth(p, cond)) {
            if (!tick(p)) break;
            if (run_body(p, b, ret)) return 1;
            Parser test = parser(p->c, condition, condition_end);
            cond = expression(&test, 1, 1);
        }
        return 0;
    }
    if (is(p, "return")) {
        next(p);
        if (!p->c->call_depth) { fail(p, "return outside function"); return 0; }
        *ret = p->t.kind == ';' || p->t.kind == END ? number(0) : expression(p, 1, 1);
        end_statement(p); return 1;
    }
    if (is(p, "let") || is(p, "var") || is(p, "string") || is(p, "number")) {
        int type = is(p, "string") ? STRING : is(p, "number") ? NUMBER : -1;
        next(p); char id[NAME_MAX]; name(p, id);
        if (private_name(id)) fail(p, "reserved identifier prefix");
        expect(p, '='); Value v = expression(p, 1, 1);
        if (type >= 0 && v.type != type) fail(p, "declaration type mismatch");
        if (!p->c->error) bind(p, id, v, 1);
        end_statement(p); return 0;
    }
    if (p->t.kind == IDENT) {
        Parser probe = *p; char id[NAME_MAX]; name(&probe, id);
        if (probe.t.kind == '=' || probe.t.kind == '[') {
            *p = probe;
            int i = find_var(p->c, id); Value *target = 0;
            if (p->t.kind == '[') {
                if (i < 0) { fail(p, "undefined indexed variable"); return 0; }
                Value container = p->c->vars[i].value;
                for (;;) {
                    next(p); Value index = expression(p, 1, 1); expect(p, ']');
                    Object *o = live(p, container); size_t n = count(p, index);
                    if (!o || n >= o->length) { fail(p, "index out of bounds"); return 0; }
                    if (p->t.kind == '[') { container = indexed(p, container, index); continue; }
                    expect(p, '='); Value rhs = expression(p, 1, 1);
                    if (!o->data) { fail(p, "assignment target was freed"); return 0; }
                    if (container.type == ARRAY) target = &((Value *)o->data)[n];
                    else {
                        if (container.type != BUFFER) fail(p, "strings are immutable; use malloc for mutable bytes");
                        int64_t byte = numeric(p, rhs);
                        if (byte < 0 || byte > 255) fail(p, "byte value out of range");
                        /* RHS can free the target buffer, so revalidate before writing. */
                        if (!o->data) fail(p, "assignment target was freed");
                        if (!p->c->error) ((unsigned char *)o->data)[n] = (unsigned char)byte;
                    }
                    if (target) { if (!o->data) fail(p, "assignment target was freed"); if (!p->c->error) *target = rhs; }
                    break;
                }
            } else {
                next(p); Value v = expression(p, 1, 1);
                if (!p->c->error) bind(p, id, v, 0);
            }
            end_statement(p); return 0;
        }
        if (probe.t.kind == '(') { expression(p, 1, 1); end_statement(p); return 0; }
        shell(p); return 0;
    }
    fail(p, "expected statement"); return 0;
}
static int statements(Parser *p, Value *returned) {
    while (p->t.kind != END && !p->c->error && !p->c->stopped) if (statement(p, returned)) return 1;
    return 0;
}
int script_run(const char *source) {
    Context *c = sys_malloc(sizeof(*c));
    if (!c) { printstr("\nscript: out of memory\n"); return -1; }
    unsigned char *bytes = (unsigned char *)c;
    for (size_t i = 0; i < sizeof(*c); i++) bytes[i] = 0;
    c->source = source;
    Parser p = parser(c, source, source + length(source));
    const char *constants[] = {"O_RDONLY", "O_WRONLY", "O_RDWR", "O_CREAT", "O_TRUNC", "O_APPEND"};
    const int values[] = {O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND};
    for (int i = 0; i < 6; i++) bind(&p, constants[i], number(values[i]), 1);
    Value result = number(0); statements(&p, &result);
    int status = c->error ? -1 : 0;
    if (c->error) {
        size_t line = 1;
        for (const char *s = source; s < c->error_at && *s; s++) if (*s == '\n') line++;
        char n[22]; decimal(n, (int64_t)line);
        printstr("\nscript line "); printstr(n); printstr(": "); printstr(c->error); printchar('\n');
    }
    for (int i = 0; i < 16; i++) if (c->files[i]) sys_close(i);
    for (int i = 0; i < 4; i++) if (c->sockets[i]) tcp_close(i);
    for (Object *o = c->objects; o;) { Object *next_o = o->next; sys_free(o->data); sys_free(o); o = next_o; }
    sys_free(c); return status;
}
