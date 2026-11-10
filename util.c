/*
 * util.c — Arena allocator, string buffer and error reporting.
 */
#include <ctype.h>
#include "tamizhi.h"
#include <errno.h>
#include <sys/stat.h>

int g_error_count = 0;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "tamizhi: out of memory\n"); exit(1); }
    return p;
}

/* ─── Diagnostics ──────────────────────────────────────────────────────── */
int g_warning_count = 0;
int g_color = 0;              /* set by main: 1 = use ANSI colours */
bool g_warnings_enabled = true;
bool g_warnings_are_errors = false;
#define MAX_ERRORS 20

static char g_hint[200];

void diag_hint(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_hint, sizeof g_hint, fmt, ap);
    va_end(ap);
}

/* Source files are read once and cached so the offending line can be shown. */
static struct { char *name; char *text; } g_src[16];
static int g_nsrc = 0;

static const char *source_of(const char *filename) {
    for (int i = 0; i < g_nsrc; i++)
        if (!strcmp(g_src[i].name, filename)) return g_src[i].text;
    if (g_nsrc >= 16) return NULL;
    char *t = read_source_file(filename, NULL);
    g_src[g_nsrc].name = strdup(filename);
    g_src[g_nsrc].text = t;
    g_nsrc++;
    return t;
}

static void show_source(const char *filename, int line, int col, int width, const char *colour) {
    const char *t = line > 0 ? source_of(filename) : NULL;
    if (!t) return;
    const char *p = t;
    for (int i = 1; i < line && p; i++) { p = strchr(p, '\n'); if (p) p++; }
    if (!p || !*p) return;
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len && p[len - 1] == '\r') len--;
    const char *dim = g_color ? "\033[34m" : "", *rst = g_color ? "\033[0m" : "";
    fprintf(stderr, "%s%5d |%s ", dim, line, rst);
    for (size_t i = 0; i < len; i++) fputc(p[i] == '\t' ? ' ' : p[i], stderr);
    fputc('\n', stderr);
    fprintf(stderr, "%s      |%s ", dim, rst);
    int pad = 0;                                   /* columns, counting UTF-8 code points */
    for (int i = 0; i < col - 1 && (size_t)i < len; i++)
        if (((unsigned char)p[i] & 0xC0) != 0x80) pad++;
    for (int i = 0; i < pad; i++) fputc(' ', stderr);
    fprintf(stderr, "%s^", colour);
    for (int i = 1; i < width; i++) fputc('~', stderr);
    fprintf(stderr, "%s\n", g_color ? "\033[0m" : "");
}

/* Length of the identifier or number starting at (line, col); 1 otherwise. */
static int token_width(const char *filename, int line, int col) {
    const char *t = source_of(filename);
    if (!t) return 1;
    const char *p = t;
    for (int i = 1; i < line && p; i++) { p = strchr(p, '\n'); if (p) p++; }
    if (!p) return 1;
    size_t n = strlen(p);
    if ((size_t)(col - 1) >= n) return 1;
    p += col - 1;
    int w = 0;
    while (isalnum((unsigned char)p[w]) || p[w] == '_') w++;
    return w ? w : 1;
}

static void vdiag(const char *filename, int line, int col, bool is_error,
                  const char *fmt, va_list ap) {
    {   /* a member access is positioned at its '.', point at the name instead */
        const char *t = source_of(filename);
        const char *p = t;
        for (int i = 1; p && i < line; i++) { p = strchr(p, '\n'); if (p) p++; }
        if (p && col >= 1 && (size_t)(col - 1) < strcspn(p, "\n") && p[col - 1] == '.') col++;
    }
    const char *colour = g_color ? (is_error ? "\033[1;31m" : "\033[1;33m") : "";
    const char *bold = g_color ? "\033[1m" : "", *rst = g_color ? "\033[0m" : "";
    fprintf(stderr, "%s%s:%d:%d:%s %s%s:%s %s", bold, filename, line, col, rst,
            colour, is_error ? "error" : "warning", rst, bold);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s\n", rst);
    show_source(filename, line, col, token_width(filename, line, col), colour);
    if (g_hint[0]) {
        fprintf(stderr, "      = %shelp:%s %s\n", g_color ? "\033[1;36m" : "",
                rst, g_hint);
        g_hint[0] = 0;
    }
}

void report_error(const char *filename, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vdiag(filename, line, col, true, fmt, ap);
    va_end(ap);
    g_error_count++;
    if (g_error_count >= MAX_ERRORS) {
        fprintf(stderr, "too many errors (%d); stopping\n", MAX_ERRORS);
        exit(1);
    }
}

void report_warning(const char *filename, int line, int col, const char *fmt, ...) {
    if (!g_warnings_enabled || (g_error_count && !g_warnings_are_errors)) { g_hint[0] = 0; return; }
    va_list ap;
    va_start(ap, fmt);
    vdiag(filename, line, col, g_warnings_are_errors, fmt, ap);
    va_end(ap);
    if (g_warnings_are_errors) g_error_count++; else g_warning_count++;
}

/* Edit distance and "did you mean" ranking for misspelt names. */
int edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (size_t i = 0; i <= la; i++) d[i][0] = (int)i;
    for (size_t j = 0; j <= lb; j++) d[0][j] = (int)j;
    for (size_t i = 1; i <= la; i++)
        for (size_t j = 1; j <= lb; j++) {
            int c = a[i - 1] == b[j - 1] ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}

const char *best_match(const char *name, const char **cands, int n) {
    const char *best = NULL;
    int bd = 99, limit = (int)strlen(name) <= 3 ? 1 : ((int)strlen(name) + 2) / 3;
    for (int i = 0; i < n; i++) {
        if (!cands[i] || !strcmp(cands[i], name)) continue;
        int d = edit_distance(name, cands[i]);
        if (d < bd) { bd = d; best = cands[i]; }
    }
    return bd <= limit ? best : NULL;
}

/* ─── Arena ────────────────────────────────────────────────────────────── */
static ArenaBlock *block_new(size_t cap) {
    ArenaBlock *b = xmalloc(sizeof(ArenaBlock));
    b->base     = xmalloc(cap);
    b->offset   = 0;
    b->capacity = cap;
    b->next     = NULL;
    return b;
}

Arena *arena_create(void) {
    Arena *a = xmalloc(sizeof(Arena));
    a->head = a->current = block_new(ARENA_BLOCK_SIZE);
    a->total_allocated = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    size = (size + 7) & ~(size_t)7;                 /* 8-byte alignment */
    ArenaBlock *b = a->current;
    if (b->offset + size > b->capacity) {
        size_t cap = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;
        ArenaBlock *nb = block_new(cap);
        b->next    = nb;
        a->current = nb;
        b          = nb;
    }
    void *p = b->base + b->offset;
    b->offset += size;
    a->total_allocated += size;
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t len) {
    char *d = arena_alloc(a, len + 1);
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

char *arena_strdup(Arena *a, const char *s) {
    return arena_strndup(a, s, strlen(s));
}

void arena_destroy(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b->base);
        free(b);
        b = next;
    }
    free(a);
}

/* ─── StrBuf ───────────────────────────────────────────────────────────── */
void strbuf_init(StrBuf *sb) {
    sb->data = xmalloc(STRBUF_INIT_CAP);
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = STRBUF_INIT_CAP;
}

static void strbuf_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    while (sb->cap < sb->len + extra + 1) sb->cap *= 2;
    sb->data = realloc(sb->data, sb->cap);
    if (!sb->data) { fprintf(stderr, "tamizhi: out of memory\n"); exit(1); }
}

void strbuf_append_n(StrBuf *sb, const char *s, size_t n) {
    strbuf_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void strbuf_append(StrBuf *sb, const char *s) {
    strbuf_append_n(sb, s, strlen(s));
}

void strbuf_appendf(StrBuf *sb, const char *fmt, ...) {
    char tmp[512];                       /* fast path: most pieces are short */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        strbuf_append_n(sb, tmp, (size_t)n);
        return;
    }
    strbuf_reserve(sb, (size_t)n);       /* slow path: format straight in */
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void strbuf_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ─── File reading ─────────────────────────────────────────────────────── */
char *read_source_file(const char *path, Arena *a) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (!S_ISREG(st.st_mode)) { errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL; return NULL; }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t size = (size_t)st.st_size;
    char *buf = a ? arena_alloc(a, size + 1) : malloc(size + 1);
    if (!buf) { fclose(f); errno = ENOMEM; return NULL; }
    if (fread(buf, 1, size, f) != size) {
        int e = errno ? errno : EIO;
        fclose(f);
        if (!a) free(buf);
        errno = e;
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    return buf;
}
