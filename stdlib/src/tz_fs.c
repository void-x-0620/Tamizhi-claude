/* fs.c — files, directories, paths and processes */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "tz_std.h"

long long tz_file_size(const char *p) {
    struct stat st; return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}
long long tz_is_dir(const char *p)  { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
long long tz_is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
long long tz_rename_file(const char *a, const char *b) { return rename(a, b) == 0; }
long long tz_copy_file(const char *a, const char *b) {
    FILE *in = fopen(a, "rb"); if (!in) return 0;
    FILE *out = fopen(b, "wb"); if (!out) { fclose(in); return 0; }
    char buf[8192]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok;
}
long long tz_make_dir(const char *p) { return mkdir(p, 0777) == 0 || errno == EEXIST; }
long long tz_remove_dir(const char *p) { return rmdir(p) == 0; }
const char *tz_list_dir(const char *p) {            /* names separated by newlines, sorted */
    DIR *d = opendir(p);
    if (!d) return NULL;
    size_t cap = 256, n = 0; char *b = (char *)malloc(cap);
    if (!b) exit(1);
    b[0] = '\0';
    char **names = NULL; size_t nn = 0, ncap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (nn == ncap) { ncap = ncap ? ncap * 2 : 16; names = (char **)realloc(names, ncap * sizeof *names); if (!names) exit(1); }
        names[nn++] = tz_dup(e->d_name, strlen(e->d_name));
    }
    closedir(d);
    for (size_t i = 1; i < nn; i++) {                /* insertion sort */
        char *k = names[i]; size_t j = i;
        while (j > 0 && strcmp(names[j - 1], k) > 0) { names[j] = names[j - 1]; j--; }
        names[j] = k;
    }
    for (size_t i = 0; i < nn; i++) {
        size_t l = strlen(names[i]);
        while (n + l + 2 > cap) { cap *= 2; b = (char *)realloc(b, cap); if (!b) exit(1); }
        memcpy(b + n, names[i], l); n += l; b[n++] = '\n'; b[n] = '\0';
        free(names[i]);
    }
    free(names);
    if (n) b[n - 1] = '\0';                          /* no trailing newline */
    return b;
}
const char *tz_current_dir(void) {
    char b[PATH_MAX];
    return getcwd(b, sizeof b) ? tz_dup(b, strlen(b)) : tz_dup("", 0);
}
long long tz_change_dir(const char *p) { return chdir(p) == 0; }
const char *tz_home_dir(void) { const char *h = getenv("HOME"); return h ? h : ""; }
const char *tz_temp_dir(void) { const char *t = getenv("TMPDIR"); return t && *t ? t : "/tmp"; }
const char *tz_path_join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (!la) return tz_dup(b, lb);
    if (!lb) return tz_dup(a, la);
    if (b[0] == '/') return tz_dup(b, lb);
    int slash = a[la - 1] == '/';
    char *r = (char *)malloc(la + lb + 2);
    if (!r) exit(1);
    memcpy(r, a, la);
    if (!slash) r[la++] = '/';
    memcpy(r + la, b, lb + 1);
    return r;
}
const char *tz_path_base(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && p[i - 1] != '/') i--;
    return tz_dup(p + i, n - i);
}
const char *tz_path_dir(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    while (n > 0 && p[n - 1] != '/') n--;
    while (n > 1 && p[n - 1] == '/') n--;
    if (n == 0) return tz_dup(".", 1);
    return tz_dup(p, n);
}
const char *tz_path_ext(const char *p) {
    const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
    const char *dot = strrchr(base, '.');
    return dot && dot != base ? tz_dup(dot, strlen(dot)) : tz_dup("", 0);
}

/* ---- process ---- */
long long tz_run_command(const char *cmd) {
    fflush(stdout);
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128 + WTERMSIG(rc);
}
const char *tz_capture_command(const char *cmd) {
    fflush(stdout);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 256, n = 0; char *b = (char *)malloc(cap); int c;
    if (!b) exit(1);
    while ((c = fgetc(p)) != EOF) {
        if (n + 1 >= cap) { cap *= 2; b = (char *)realloc(b, cap); if (!b) exit(1); }
        b[n++] = (char)c;
    }
    b[n] = '\0';
    pclose(p);
    return b;
}
long long tz_get_pid(void) { return (long long)getpid(); }
const char *tz_hostname(void) {
    char b[256];
    return gethostname(b, sizeof b) == 0 ? tz_dup(b, strnlen(b, sizeof b)) : tz_dup("", 0);
}
