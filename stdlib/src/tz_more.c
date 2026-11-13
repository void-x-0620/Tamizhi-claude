/* more.c — extra string, math, time and io functions */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "tz_std.h"

/* ---- io ---- */
const char *tz_read_all(void) {
    size_t cap = 256, n = 0; char *b = (char *)malloc(cap); int c;
    if (!b) exit(1);
    while ((c = getchar()) != EOF) {
        if (n + 1 >= cap) { cap *= 2; b = (char *)realloc(b, cap); if (!b) exit(1); }
        b[n++] = (char)c;
    }
    b[n] = '\0';
    return b;
}
void tz_print_str(const char *s) { fputs(s ? s : "", stdout); }
void tz_eprint(const char *s) { fflush(stdout); fputs(s ? s : "", stderr); }
long long tz_remove_file(const char *p) { return remove(p) == 0; }

/* ---- string ---- */
long long tz_char_at(const char *s, long long i) {
    long long n = (long long)strlen(s);
    return i >= 0 && i < n ? (unsigned char)s[i] : -1;
}
long long tz_ord(const char *s) { return s[0] ? (unsigned char)s[0] : -1; }
const char *tz_str_reverse(const char *s) {
    size_t n = strlen(s); char *r = tz_dup(s, n);
    for (size_t i = 0; i < n / 2; i++) { char t = r[i]; r[i] = r[n - 1 - i]; r[n - 1 - i] = t; }
    return r;
}
long long tz_str_count(const char *s, const char *sub) {
    size_t m = strlen(sub); long long c = 0;
    if (!m) return 0;
    for (const char *p = s; (p = strstr(p, sub)); p += m) c++;
    return c;
}
static const char *pad(const char *s, long long w, const char *fill, int left) {
    size_t n = strlen(s), f = strlen(fill);
    if (w <= (long long)n || !f) return tz_dup(s, n);
    size_t add = (size_t)w - n;
    char *r = (char *)malloc((size_t)w + 1);
    if (!r) exit(1);
    char *p = r;
    if (left) { for (size_t i = 0; i < add; i++) *p++ = fill[i % f]; memcpy(p, s, n); p += n; }
    else      { memcpy(p, s, n); p += n; for (size_t i = 0; i < add; i++) *p++ = fill[i % f]; }
    *p = '\0';
    return r;
}
const char *tz_str_pad_left(const char *s, long long w, const char *fill)  { return pad(s, w, fill, 1); }
const char *tz_str_pad_right(const char *s, long long w, const char *fill) { return pad(s, w, fill, 0); }
long long tz_str_cmp(const char *a, const char *b) {
    int c = strcmp(a, b); return c < 0 ? -1 : c > 0 ? 1 : 0;
}
long long tz_str_is_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}
long long tz_str_is_alpha(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isalpha((unsigned char)*s)) return 0;
    return 1;
}
const char *tz_str_capitalize(const char *s) {
    char *r = tz_dup(s, strlen(s));
    if (r[0]) r[0] = (char)toupper((unsigned char)r[0]);
    return r;
}
const char *tz_to_hex(long long v) {
    char b[32]; snprintf(b, sizeof b, "%llx", (unsigned long long)v); return tz_dup(b, strlen(b));
}
const char *tz_to_bin(long long v) {
    char b[80]; int n = 0; unsigned long long u = (unsigned long long)v;
    if (!u) b[n++] = '0';
    while (u) { b[n++] = (char)('0' + (u & 1)); u >>= 1; }
    for (int i = 0; i < n / 2; i++) { char t = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = t; }
    return tz_dup(b, (size_t)n);
}
long long tz_parse_int_base(const char *s, long long base) {
    if (base < 2 || base > 36) return 0;
    return strtoll(s, NULL, (int)base);
}

/* ---- math ---- */
double tz_asin(double x) { return asin(x); }
double tz_acos(double x) { return acos(x); }
double tz_atan(double x) { return atan(x); }
double tz_atan2(double y, double x) { return atan2(y, x); }
double tz_sinh(double x) { return sinh(x); }
double tz_cosh(double x) { return cosh(x); }
double tz_tanh(double x) { return tanh(x); }
double tz_log10(double x) { return log10(x); }
double tz_log2(double x) { return log2(x); }
double tz_hypot(double x, double y) { return hypot(x, y); }
double tz_trunc(double x) { return trunc(x); }
double tz_fmod(double x, double y) { return fmod(x, y); }
double tz_cbrt(double x) { return cbrt(x); }
double tz_e(void) { return 2.71828182845904523536; }
double tz_inf(void) { return INFINITY; }
long long tz_is_nan(double x) { return isnan(x) != 0; }
double tz_deg_to_rad(double d) { return d * 3.14159265358979323846 / 180.0; }
double tz_rad_to_deg(double r) { return r * 180.0 / 3.14159265358979323846; }
double tz_minf(double a, double b) { return a < b ? a : b; }
double tz_maxf(double a, double b) { return a > b ? a : b; }
long long tz_clamp(long long v, long long lo, long long hi) { return v < lo ? lo : v > hi ? hi : v; }
double tz_clampf(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
long long tz_sign(long long v) { return v < 0 ? -1 : v > 0; }
long long tz_gcd(long long a, long long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long long t = a % b; a = b; b = t; }
    return a;
}
long long tz_lcm(long long a, long long b) {
    if (!a || !b) return 0;
    long long g = tz_gcd(a, b);
    long long r = a / g * b;
    return r < 0 ? -r : r;
}
long long tz_ipow(long long b, long long e) {
    long long r = 1;
    if (e < 0) return 0;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return r;
}
long long tz_factorial(long long n) {
    long long r = 1;
    for (long long i = 2; i <= n && i <= 20; i++) r *= i;
    return n < 0 ? 0 : r;
}
long long tz_is_prime(long long n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    for (long long i = 3; i * i <= n; i += 2) if (n % i == 0) return 0;
    return 1;
}

/* ---- time ---- */
const char *tz_format_time(long long ts, const char *fmt) {
    time_t t = (time_t)ts; struct tm tmv; char b[256];
    localtime_r(&t, &tmv);
    size_t n = strftime(b, sizeof b, fmt, &tmv);
    return tz_dup(b, n);
}
const char *tz_now_iso(void) { return tz_format_time((long long)time(NULL), "%Y-%m-%dT%H:%M:%S"); }
/* 0 year 1 month 2 day 3 hour 4 minute 5 second 6 weekday (0=Sunday) 7 day of year */
long long tz_time_part(long long ts, long long which) {
    time_t t = (time_t)ts; struct tm tmv;
    localtime_r(&t, &tmv);
    switch (which) {
        case 0: return tmv.tm_year + 1900; case 1: return tmv.tm_mon + 1;
        case 2: return tmv.tm_mday;        case 3: return tmv.tm_hour;
        case 4: return tmv.tm_min;         case 5: return tmv.tm_sec;
        case 6: return tmv.tm_wday;        case 7: return tmv.tm_yday + 1;
    }
    return -1;
}
void tz_rand_seed_now(void) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); }
long long tz_year(long long ts)    { return tz_time_part(ts, 0); }
long long tz_month(long long ts)   { return tz_time_part(ts, 1); }
long long tz_day(long long ts)     { return tz_time_part(ts, 2); }
long long tz_hour(long long ts)    { return tz_time_part(ts, 3); }
long long tz_minute(long long ts)  { return tz_time_part(ts, 4); }
long long tz_second(long long ts)  { return tz_time_part(ts, 5); }
long long tz_weekday(long long ts) { return tz_time_part(ts, 6); }
