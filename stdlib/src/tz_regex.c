/* regex.c — POSIX extended regular expressions */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include "tz_std.h"

long long tz_regex_match(const char *pattern, const char *s) {
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) return -1;   /* -1: bad pattern */
    int rc = regexec(&re, s, 0, NULL, 0);
    regfree(&re);
    return rc == 0;
}
const char *tz_regex_find(const char *pattern, const char *s) {
    regex_t re; regmatch_t m[1];
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return tz_dup("", 0);
    const char *r = regexec(&re, s, 1, m, 0) == 0
        ? tz_dup(s + m[0].rm_so, (size_t)(m[0].rm_eo - m[0].rm_so)) : tz_dup("", 0);
    regfree(&re);
    return r;
}
const char *tz_regex_replace(const char *pattern, const char *s, const char *repl) {
    regex_t re; regmatch_t m[1];
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return tz_dup(s, strlen(s));
    size_t cap = strlen(s) + 64, n = 0, rl = strlen(repl);
    char *out = (char *)malloc(cap);
    if (!out) exit(1);
    const char *p = s;
    int flags = 0;
    while (*p || !flags) {
        if (regexec(&re, p, 1, m, flags) != 0) break;
        size_t pre = (size_t)m[0].rm_so, len = (size_t)(m[0].rm_eo - m[0].rm_so);
        while (n + pre + rl + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) exit(1); }
        memcpy(out + n, p, pre); n += pre;
        memcpy(out + n, repl, rl); n += rl;
        if (len == 0) {                                   /* empty match: step over one char */
            if (!p[pre]) { p += pre; break; }
            out[n++] = p[pre]; p += pre + 1;
        } else p += pre + len;
        flags = REG_NOTBOL;
    }
    size_t rest = strlen(p);
    while (n + rest + 1 > cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) exit(1); }
    memcpy(out + n, p, rest + 1);
    regfree(&re);
    return out;
}
