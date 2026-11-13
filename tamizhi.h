/*
 * tamizhi.h — Shared types and utilities for the Tamizhi compiler.
 *
 * Memory design: every AST node, symbol and interned string is carved out of
 * an Arena (bump allocator). The whole compilation is released with one
 * arena_destroy() call, so there is no per-node free() and no fragmentation.
 */
#ifndef TAMIZHI_H
#define TAMIZHI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#define TAMIZHI_VERSION  "2.6.0"
#define ARENA_BLOCK_SIZE (256 * 1024)
#define STRBUF_INIT_CAP  4096

/* ─── Arena allocator ──────────────────────────────────────────────────── */
typedef struct ArenaBlock {
    char              *base;
    size_t             offset;
    size_t             capacity;
    struct ArenaBlock *next;
} ArenaBlock;

typedef struct {
    ArenaBlock *head;      /* first block (needed to free the chain)  */
    ArenaBlock *current;   /* block currently being bump-allocated    */
    size_t      total_allocated;
} Arena;

Arena *arena_create(void);
void  *arena_alloc(Arena *a, size_t size);
char  *arena_strndup(Arena *a, const char *s, size_t len);
char  *arena_strdup(Arena *a, const char *s);
void   arena_destroy(Arena *a);

/* ─── Growable string buffer (code generator output) ───────────────────── */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

void strbuf_init(StrBuf *sb);
void strbuf_append(StrBuf *sb, const char *s);
void strbuf_append_n(StrBuf *sb, const char *s, size_t n);
void strbuf_appendf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void strbuf_free(StrBuf *sb);

/* Read a whole file. Returns a NUL-terminated buffer (arena-owned if `a` is
 * non-NULL, otherwise malloc'd) or NULL with errno set. Never prints. */
char *read_source_file(const char *path, Arena *a);

/* ─── Diagnostics ──────────────────────────────────────────────────────── */
extern int g_error_count, g_warning_count, g_color;
extern bool g_warnings_enabled, g_warnings_are_errors;

/* Attach a "help:" line to the next diagnostic. */
void diag_hint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void report_warning(const char *filename, int line, int col,
                    const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int edit_distance(const char *a, const char *b);
/* Closest candidate to name (or NULL if nothing is close enough). */
const char *best_match(const char *name, const char **cands, int n);

void report_error(const char *filename, int line, int col,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));

#endif /* TAMIZHI_H */
