/* json.c — JSON as text: validate, query by path ("a.b[2].c"), pretty-print, escape */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tz_std.h"

#define MAXDEPTH 200
static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

/* Skip one value starting at p; returns the pointer after it, or NULL if malformed. */
static const char *skip_value(const char *p, int depth);
static const char *skip_string(const char *p) {
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        if ((unsigned char)*p < 0x20) return NULL;
        if (*p == '\\') {
            p++;
            if (*p == 'u') { for (int i = 1; i <= 4; i++) if (!isxdigit((unsigned char)p[i])) return NULL; p += 4; }
            else if (!*p || !strchr("\"\\/bfnrt", *p)) return NULL;
        }
    }
    return *p == '"' ? p + 1 : NULL;
}
static const char *skip_value(const char *p, int depth) {
    if (depth > MAXDEPTH) return NULL;
    p = ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            p = ws(p);
            if (obj) {
                p = skip_string(p); if (!p) return NULL;
                p = ws(p); if (*p != ':') return NULL;
                p++;
            }
            p = skip_value(p, depth + 1); if (!p) return NULL;
            p = ws(p);
            if (*p == ',') { p++; continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    if (*p == '-' || isdigit((unsigned char)*p)) {
        const char *q = p;
        if (*q == '-') q++;
        if (!isdigit((unsigned char)*q)) return NULL;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.') { q++; if (!isdigit((unsigned char)*q)) return NULL; while (isdigit((unsigned char)*q)) q++; }
        if (*q == 'e' || *q == 'E') {
            q++; if (*q == '+' || *q == '-') q++;
            if (!isdigit((unsigned char)*q)) return NULL;
            while (isdigit((unsigned char)*q)) q++;
        }
        return q;
    }
    return NULL;
}

long long tz_json_valid(const char *s) {
    const char *p = skip_value(s, 0);
    return p && *ws(p) == '\0';
}

/* Decode the JSON string literal at p (opening quote) into a malloc'd C string. */
static char *decode_string(const char *p, const char *end) {
    char *r = (char *)malloc((size_t)(end - p) + 1), *o = r;
    if (!r) exit(1);
    for (p++; p < end - 1; p++) {
        if (*p != '\\') { *o++ = *p; continue; }
        p++;
        switch (*p) {
            case 'n': *o++ = '\n'; break; case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break; case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'u': {
                unsigned cp = (unsigned)strtoul((char[]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
                p += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {     /* surrogate pair */
                    unsigned lo = (unsigned)strtoul((char[]){p[3], p[4], p[5], p[6], 0}, NULL, 16);
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6;
                }
                if (cp < 0x80) *o++ = (char)cp;
                else if (cp < 0x800) { *o++ = (char)(0xc0 | cp >> 6); *o++ = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { *o++ = (char)(0xe0 | cp >> 12); *o++ = (char)(0x80 | (cp >> 6 & 63)); *o++ = (char)(0x80 | (cp & 63)); }
                else { *o++ = (char)(0xf0 | cp >> 18); *o++ = (char)(0x80 | (cp >> 12 & 63)); *o++ = (char)(0x80 | (cp >> 6 & 63)); *o++ = (char)(0x80 | (cp & 63)); }
                break;
            }
            default: *o++ = *p;
        }
    }
    *o = '\0';
    return r;
}

/* Follow a path like  users[0].name  starting at the value in s. Sets start and end. */
static int locate(const char *s, const char *path, const char **start, const char **end) {
    const char *cur = ws(s);
    const char *stop = skip_value(cur, 0);
    if (!stop) return 0;
    while (*path) {
        if (*path == '.') { path++; continue; }
        if (*path == '[') {
            char *e; long idx = strtol(path + 1, &e, 10);
            if (*e != ']' || idx < 0 || *cur != '[') return 0;
            path = e + 1;
            const char *p = ws(cur + 1); long i = 0; int found = 0;
            while (*p != ']') {
                const char *vend = skip_value(p, 0);
                if (!vend) return 0;
                if (i == idx) { cur = ws(p); stop = vend; found = 1; break; }
                p = ws(vend); if (*p == ',') p = ws(p + 1);
                i++;
            }
            if (!found) return 0;
        } else {
            size_t kl = strcspn(path, ".[");
            if (*cur != '{') return 0;
            const char *p = ws(cur + 1); int found = 0;
            while (*p == '"') {
                const char *kend = skip_string(p);
                if (!kend) return 0;
                char *key = decode_string(p, kend);
                int hit = strlen(key) == kl && !strncmp(key, path, kl);
                free(key);
                p = ws(kend); if (*p != ':') return 0;
                p = ws(p + 1);
                const char *vend = skip_value(p, 0);
                if (!vend) return 0;
                if (hit) { cur = p; stop = vend; found = 1; break; }
                p = ws(vend); if (*p == ',') p = ws(p + 1);
            }
            if (!found) return 0;
            path += kl;
        }
    }
    *start = cur; *end = stop;
    return 1;
}

/* "string" "number" "bool" "null" "object" "array", or "" if the path does not exist */
const char *tz_json_type(const char *s, const char *path) {
    const char *a, *b;
    if (!locate(s, path, &a, &b)) return "";
    switch (*a) {
        case '"': return "string"; case '{': return "object"; case '[': return "array";
        case 't': case 'f': return "bool"; case 'n': return "null";
    }
    return "number";
}
/* the value as text: strings decoded, everything else as written; "" when missing */
const char *tz_json_get(const char *s, const char *path) {
    const char *a, *b;
    if (!locate(s, path, &a, &b)) return tz_dup("", 0);
    if (*a == '"') return decode_string(a, b);
    return tz_dup(a, (size_t)(b - a));
}
long long tz_json_has(const char *s, const char *path) {
    const char *a, *b; return locate(s, path, &a, &b);
}
/* number of elements of an array or members of an object; -1 otherwise */
long long tz_json_len(const char *s, const char *path) {
    const char *a, *b;
    if (!locate(s, path, &a, &b) || (*a != '[' && *a != '{')) return -1;
    long long n = 0; const char *p = ws(a + 1);
    char close = *a == '{' ? '}' : ']';
    while (*p != close) {
        if (*a == '{') { p = skip_string(p); p = ws(p) + 1; }
        p = skip_value(p, 0); p = ws(p);
        n++;
        if (*p == ',') p = ws(p + 1);
    }
    return n;
}
/* the keys of an object, one per line */
const char *tz_json_keys(const char *s, const char *path) {
    const char *a, *b;
    if (!locate(s, path, &a, &b) || *a != '{') return tz_dup("", 0);
    size_t cap = 64, n = 0; char *out = (char *)malloc(cap);
    if (!out) exit(1);
    out[0] = '\0';
    const char *p = ws(a + 1);
    while (*p == '"') {
        const char *kend = skip_string(p);
        char *k = decode_string(p, kend); size_t kl = strlen(k);
        while (n + kl + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) exit(1); }
        if (n) out[n++] = '\n';
        memcpy(out + n, k, kl + 1); n += kl; free(k);
        p = ws(kend) + 1; p = skip_value(p, 0); p = ws(p);
        if (*p == ',') p = ws(p + 1);
    }
    return out;
}

/* escape / quote */
const char *tz_json_escape(const char *s) {
    size_t n = strlen(s), o = 0;
    char *r = (char *)malloc(n * 6 + 1);
    if (!r) exit(1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': r[o++] = '\\'; r[o++] = '"'; break;
            case '\\': r[o++] = '\\'; r[o++] = '\\'; break;
            case '\n': r[o++] = '\\'; r[o++] = 'n'; break;
            case '\t': r[o++] = '\\'; r[o++] = 't'; break;
            case '\r': r[o++] = '\\'; r[o++] = 'r'; break;
            default:
                if (c < 0x20) o += (size_t)sprintf(r + o, "\\u%04x", c);
                else r[o++] = (char)c;
        }
    }
    r[o] = '\0';
    return r;
}
const char *tz_json_quote(const char *s) {
    const char *e = tz_json_escape(s);
    size_t n = strlen(e);
    char *r = (char *)malloc(n + 3);
    if (!r) exit(1);
    r[0] = '"'; memcpy(r + 1, e, n); r[n + 1] = '"'; r[n + 2] = '\0';
    free((void *)e);
    return r;
}

/* pretty / minify: re-emit the text with or without indentation */
static const char *reformat(const char *s, int indent) {
    if (!tz_json_valid(s)) return NULL;
    size_t cap = strlen(s) * 3 + 64, o = 0; char *r = (char *)malloc(cap);
    if (!r) exit(1);
    int depth = 0, in_str = 0;
#define PUT(ch) do { if (o + 2 * (size_t)depth * (size_t)indent + 8 >= cap) { cap *= 2; r = (char *)realloc(r, cap); if (!r) exit(1); } r[o++] = (ch); } while (0)
#define NL() do { if (indent) { PUT('\n'); for (int k = 0; k < depth * indent; k++) PUT(' '); } } while (0)
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (in_str) { PUT(c); if (c == '\\') { p++; PUT(*p); } else if (c == '"') in_str = 0; continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '"') { in_str = 1; PUT(c); }
        else if (c == '{' || c == '[') {
            PUT(c);
            const char *q = ws(p + 1);
            if (*q == (c == '{' ? '}' : ']')) { PUT(*q); p = q; }
            else { depth++; NL(); }
        }
        else if (c == '}' || c == ']') { depth--; NL(); PUT(c); }
        else if (c == ',') { PUT(c); NL(); }
        else if (c == ':') { PUT(c); if (indent) PUT(' '); }
        else PUT(c);
    }
    r[o] = '\0';
    return r;
}
const char *tz_json_pretty(const char *s) { const char *r = reformat(s, 2); return r ? r : tz_dup(s, strlen(s)); }
const char *tz_json_minify(const char *s) { const char *r = reformat(s, 0); return r ? r : tz_dup(s, strlen(s)); }
