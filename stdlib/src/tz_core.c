/* core.c — io, string, math, os and time functions of the Tamizhi standard library */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include "tz_std.h"

char *tz_dup(const char *s, size_t n) {
    char *r = (char *)malloc(n + 1);
    if (!r) { fputs("tamizhi: out of memory\n", stderr); exit(1); }
    memcpy(r, s, n); r[n] = '\0';
    return r;
}

/* ---- io ---- */
const char *tz_read_line(void) {
    size_t cap = 64, n = 0; char *b = (char *)malloc(cap); int c;
    if (!b) exit(1);
    while ((c = getchar()) != EOF && c != '\n') {
        if (n + 1 >= cap) { cap *= 2; b = (char *)realloc(b, cap); if (!b) exit(1); }
        b[n++] = (char)c;
    }
    if (n && b[n - 1] == '\r') n--;
    b[n] = '\0';
    return b;
}
long long tz_read_int(void) { return strtoll(tz_read_line(), NULL, 10); }
double tz_read_float(void) { return strtod(tz_read_line(), NULL); }
const char *tz_read_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1); if (!b) exit(1);
    size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    b[got] = '\0'; return b;
}
static int tz_write_mode(const char *path, const char *text, const char *mode) {
    FILE *f = fopen(path, mode); if (!f) return 0;
    int ok = fputs(text, f) >= 0; fclose(f); return ok;
}
long long tz_write_file(const char *p, const char *t)  { return tz_write_mode(p, t, "wb"); }
long long tz_append_file(const char *p, const char *t) { return tz_write_mode(p, t, "ab"); }
long long tz_file_exists(const char *p) { return access(p, F_OK) == 0; }

/* ---- string ---- */
long long tz_str_len(const char *s) { return (long long)strlen(s); }
const char *tz_str_sub(const char *s, long long start, long long len) {
    long long n = (long long)strlen(s);
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len < 0) len = 0;
    if (len > n - start) len = n - start;
    return tz_dup(s + start, (size_t)len);
}
long long tz_str_find(const char *s, const char *needle) {
    const char *p = strstr(s, needle); return p ? (long long)(p - s) : -1;
}
const char *tz_str_upper(const char *s) {
    char *r = tz_dup(s, strlen(s));
    for (char *p = r; *p; p++) *p = (char)toupper((unsigned char)*p);
    return r;
}
const char *tz_str_lower(const char *s) {
    char *r = tz_dup(s, strlen(s));
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}
const char *tz_str_trim(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return tz_dup(s, n);
}
long long tz_str_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
long long tz_str_ends(const char *s, const char *p) {
    size_t a = strlen(s), b = strlen(p); return b <= a && !strcmp(s + a - b, p);
}
const char *tz_str_replace(const char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to), cnt = 0;
    if (!fl) return tz_dup(s, strlen(s));
    for (const char *p = s; (p = strstr(p, from)); p += fl) cnt++;
    char *r = (char *)malloc(strlen(s) + cnt * (tl > fl ? tl - fl : 0) + 1), *o = r;
    if (!r) exit(1);
    const char *p;
    while ((p = strstr(s, from))) { memcpy(o, s, (size_t)(p - s)); o += p - s; memcpy(o, to, tl); o += tl; s = p + fl; }
    strcpy(o, s);
    return r;
}
const char *tz_str_repeat(const char *s, long long n) {
    size_t l = strlen(s); if (n < 0) n = 0;
    char *r = (char *)malloc(l * (size_t)n + 1), *o = r; if (!r) exit(1);
    for (long long i = 0; i < n; i++) { memcpy(o, s, l); o += l; }
    *o = '\0'; return r;
}
const char *tz_chr(long long c) { char b[2] = { (char)c, 0 }; return tz_dup(b, 1); }
const char *tz_int_to_str(long long v) { char b[32]; snprintf(b, sizeof b, "%lld", v); return tz_dup(b, strlen(b)); }
const char *tz_float_to_str(double v) { char b[64]; snprintf(b, sizeof b, "%g", v); return tz_dup(b, strlen(b)); }
long long tz_parse_int(const char *s) { return strtoll(s, NULL, 10); }
double tz_parse_float(const char *s) { return strtod(s, NULL); }

/* ---- math ---- */
double tz_sqrt(double x) { return sqrt(x); }
double tz_pow(double x, double y) { return pow(x, y); }
double tz_sin(double x) { return sin(x); }
double tz_cos(double x) { return cos(x); }
double tz_tan(double x) { return tan(x); }
double tz_log(double x) { return log(x); }
double tz_exp(double x) { return exp(x); }
double tz_floor(double x) { return floor(x); }
double tz_ceil(double x) { return ceil(x); }
double tz_round(double x) { return round(x); }
double tz_fabs(double x) { return fabs(x); }
long long tz_abs(long long x) { return x < 0 ? -x : x; }
long long tz_min(long long a, long long b) { return a < b ? a : b; }
long long tz_max(long long a, long long b) { return a > b ? a : b; }
double tz_pi(void) { return 3.14159265358979323846; }

/* ---- os ---- */
long long tz_arg_count(void) { return tz_argc; }
const char *tz_arg_get(long long i) { return i >= 0 && i < tz_argc ? tz_argv[i] : ""; }
void tz_exit_program(long long code) { fflush(stdout); exit((int)code); }
const char *tz_env_get(const char *n) { const char *v = getenv(n); return v ? v : ""; }

/* ---- time and random ---- */
long long tz_clock_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
long long tz_time_now(void) { return (long long)time(NULL); }
void tz_sleep_ms(long long ms) {
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL);
}
void tz_rand_seed(long long n) { srand((unsigned)n); }
long long tz_rand_int(long long lo, long long hi) {
    if (hi < lo) { long long t = lo; lo = hi; hi = t; }
    return lo + (long long)(((double)rand() / ((double)RAND_MAX + 1.0)) * (double)(hi - lo + 1));
}
double tz_rand_float(void) { return (double)rand() / ((double)RAND_MAX + 1.0); }
