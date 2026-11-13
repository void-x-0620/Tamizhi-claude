/*
 * main.c — Tamizhi compiler driver.
 *
 *   tamizhi <source.tz> [extra.c|.o|.a ...] [-o out] [-S] [-c] [-O0..3] [-v]
 *           [-Idir] [-Ldir] [-llib] [--gc|--no-gc] [--no-checks] [--cc=CMD]
 *
 * Pipeline: read -> lex+parse -> semantic analysis -> C codegen -> host cc.
 * The generated C is streamed to the host compiler over a pipe, so no
 * temporary files are created and no shell is involved.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "codegen.h"

/* Small growable list of borrowed strings (command-line arguments). */
typedef struct { const char **v; size_t n, cap; } StrList;

static bool list_push(StrList *l, const char *s) {
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 8;
        const char **nv = realloc(l->v, nc * sizeof *nv);
        if (!nv) return false;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = s;
    return true;
}

typedef struct {
    StrList     inc;         /* -I<dir>                         */
    StrList     libdir;      /* -L<dir>                         */
    StrList     lib;         /* -l<name>                        */
    StrList     extra;       /* extra .c/.o/.a files to link    */
    bool        gc;          /* --gc (default) / --no-gc        */
    bool        checks;      /* runtime checks (default) / --no-checks */
    bool        lines, debug; /* #line directives / pass -g to the C compiler */
    const char *input;
    const char *output;      /* NULL = default for the chosen mode */
    const char *cc;
    int         opt;
    bool        emit_c;      /* -S */
    bool        emit_obj;    /* -c */
    bool        verbose;
    bool        dump_tokens;
} Config;

static void usage(const char *prog) {
    fprintf(stderr,
        "Tamizhi compiler " TAMIZHI_VERSION "\n"
        "usage: %s <source.tz> [options]\n\n"
        "  -o <file>        output file (default: a.out; with -S: stdout)\n"
        "  -S               emit the generated C source instead of a binary\n"
        "  -c               compile to an object file\n"
        "  -O<0-3>          optimisation level for the host compiler (default 2)\n"
        "  -v               print per-stage timings\n"
        "  -I<dir>          header search directory for C bindings\n"
        "  -L<dir>          library search directory\n"
        "  -l<name>         link a C library (e.g. -lm, -lsqlite3)\n"
        "  file.c|.o|.a     extra C code to compile and link in\n"
        "  --gc             garbage-collect heap data with Boehm GC (default;\n"
        "                   needs libgc only if the program allocates)\n"
        "  --no-gc          use plain malloc, never freed (no libgc needed)\n"
        "  -g               debug info: gdb and cc messages show .tz lines (--lines: #line only)\n"
        "  --no-warn        hide warnings (unused variables)   -Werror  make them errors\n"
        "  --color=WHEN     auto (default), always or never\n"
        "  --no-checks      drop the runtime null / bounds / divide-by-zero checks\n"
        "  --dump-tokens    print the token stream and exit\n"
        "  --cc=<cmd>       host C compiler (default: cc)\n"
        "  --version        print the version\n"
        "  -h, --help       this message\n",
        prog);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static char *read_file(const char *path) {
    char *buf = read_source_file(path, NULL);
    if (!buf) perror(path);
    return buf;
}

/* Option value, either attached (-lfoo) or the next argument (-l foo). */
static const char *opt_value(int argc, char **argv, int *i, const char *a) {
    if (a[2]) return a + 2;
    if (++*i >= argc) { fprintf(stderr, "%s needs an argument\n", a); return NULL; }
    return argv[*i];
}

static bool has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcmp(s + n - m, suf);
}

static const char *tfmt_warn(void) {
    static char b[48];
    snprintf(b, sizeof b, " and %d warning(s)", g_warning_count);
    return b;
}

static bool parse_args(int argc, char **argv, Config *c) {
    memset(c, 0, sizeof *c);
    g_color = isatty(2) && !getenv("NO_COLOR");
    c->cc = "cc";
    c->opt = 2;
    c->gc = true;
    c->checks = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); exit(0); }
        else if (!strcmp(a, "--version")) { puts("tamizhi " TAMIZHI_VERSION); exit(0); }
        else if (!strcmp(a, "-S")) c->emit_c = true;
        else if (!strcmp(a, "-c")) c->emit_obj = true;
        else if (!strcmp(a, "-v")) c->verbose = true;
        else if (!strcmp(a, "--dump-tokens")) c->dump_tokens = true;
        else if (!strcmp(a, "--gc")) c->gc = true;
        else if (!strcmp(a, "--no-gc")) c->gc = false;
        else if (!strcmp(a, "--no-checks")) c->checks = false;
        else if (!strcmp(a, "--no-warn")) g_warnings_enabled = false;
        else if (!strcmp(a, "-g")) { c->debug = true; c->lines = true; }
        else if (!strcmp(a, "--lines")) c->lines = true;
        else if (!strcmp(a, "-Werror")) g_warnings_are_errors = true;
        else if (!strcmp(a, "--color=always")) g_color = 1;
        else if (!strcmp(a, "--color=never"))  g_color = 0;
        else if (!strcmp(a, "--color=auto"))   g_color = isatty(2) && !getenv("NO_COLOR");
        else if (!strcmp(a, "--checks")) c->checks = true;
        else if (!strncmp(a, "-I", 2) || !strncmp(a, "-L", 2) || !strncmp(a, "-l", 2)) {
            const char *v = opt_value(argc, argv, &i, a);
            if (!v) return false;
            char *flag = malloc(strlen(v) + 3);       /* "-X" + value */
            if (!flag) { fprintf(stderr, "out of memory\n"); return false; }
            sprintf(flag, "-%c%s", a[1], v);
            StrList *l = a[1] == 'I' ? &c->inc : a[1] == 'L' ? &c->libdir : &c->lib;
            if (!list_push(l, flag)) { fprintf(stderr, "out of memory\n"); return false; }
        }
        else if (!strcmp(a, "-o")) {
            if (++i >= argc) { fprintf(stderr, "-o needs an argument\n"); return false; }
            c->output = argv[i];
        } else if (!strncmp(a, "-O", 2) && a[2] >= '0' && a[2] <= '3' && !a[3]) {
            c->opt = a[2] - '0';
        } else if (!strncmp(a, "--cc=", 5) && a[5]) {
            c->cc = a + 5;
        } else if (a[0] != '-') {
            if (has_suffix(a, ".c") || has_suffix(a, ".o") || has_suffix(a, ".a") ||
                has_suffix(a, ".so")) {
                if (!list_push(&c->extra, a)) { fprintf(stderr, "out of memory\n"); return false; }
            } else {
                if (c->input) { fprintf(stderr, "only one Tamizhi source file is supported\n"); return false; }
                c->input = a;
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            return false;
        }
    }
    if (!c->input) { usage(argv[0]); return false; }
    if (c->emit_c && c->emit_obj) {
        fprintf(stderr, "-S and -c cannot be combined\n");
        return false;
    }
    return true;
}

static void dump_tokens(const char *src, const char *file, Arena *arena) {
    Lexer L;
    lexer_init(&L, src, file, arena);
    printf("%-18s %-16s %s\n", "TOKEN", "LEXEME", "LINE:COL");
    for (;;) {
        Token t = lexer_next(&L);
        char lex[40];
        int n = t.length < 39 ? t.length : 39;
        memcpy(lex, t.start, (size_t)n);
        lex[n] = '\0';
        for (char *p = lex; *p; p++) if (*p == '\n') *p = ' ';
        printf("%-18s %-16s %d:%d\n", token_type_name(t.type), lex, t.line, t.col);
        if (t.type == TOK_EOF) break;
    }
}

/* Stream `code` into `cc -x c -` and wait for it. Returns the exit status. */
static int run_cc(const Config *c, const char *out, const CodeGen *cg) {
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); return 1; }

    char optflag[8];
    snprintf(optflag, sizeof optflag, "-O%d", c->opt);
    size_t cap = 16 + c->inc.n + c->extra.n + c->libdir.n + c->lib.n;
    const char **argv = malloc(cap * sizeof *argv);
    if (!argv) { fprintf(stderr, "out of memory\n"); return 1; }
    size_t n = 0;
    argv[n++] = c->cc;
    argv[n++] = optflag;
    if (c->debug) argv[n++] = "-g";
    if (!c->verbose) argv[n++] = "-w";
    if (c->emit_obj) argv[n++] = "-c";
    argv[n++] = "-o";
    argv[n++] = out;
    for (size_t i = 0; i < c->inc.n; i++) argv[n++] = c->inc.v[i];
    argv[n++] = "-x";
    argv[n++] = "c";
    argv[n++] = "-";
    if (!c->emit_obj) {
        argv[n++] = "-x";                       /* extras: let cc pick by suffix */
        argv[n++] = "none";
        for (size_t i = 0; i < c->extra.n; i++)  argv[n++] = c->extra.v[i];
        for (size_t i = 0; i < c->libdir.n; i++) argv[n++] = c->libdir.v[i];
        for (size_t i = 0; i < c->lib.n; i++)    argv[n++] = c->lib.v[i];
        if (cg->gc && cg->uses_heap) argv[n++] = "-lgc";
        argv[n++] = "-lm";
    }
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); free(argv); return 1; }
    if (pid == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        perror(argv[0]);
        _exit(127);
    }

    close(fds[0]);
    const char *p = cg->out.data;
    size_t left = cg->out.len;
    while (left > 0) {
        ssize_t w = write(fds[1], p, left);
        if (w < 0) break;                       /* cc died early; see status */
        p += w;
        left -= (size_t)w;
    }
    close(fds[1]);

    free(argv);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static char *default_object_name(const char *input) {
    size_t n = strlen(input);
    char *out = malloc(n + 3);
    if (!out) return NULL;
    memcpy(out, input, n + 1);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    strcat(out, ".o");
    return out;
}

static int real_main(int argc, char **argv) {
    Config cfg;
    if (!parse_args(argc, argv, &cfg)) return 2;
    signal(SIGPIPE, SIG_IGN);

    double t0 = now_ms();
    char *source = read_file(cfg.input);
    if (!source) return 1;

    Arena *arena = arena_create();

    if (cfg.dump_tokens) {
        dump_tokens(source, cfg.input, arena);
        int rc = g_error_count ? 1 : 0;
        arena_destroy(arena);
        free(source);
        return rc;
    }

    /* ── Front end: lex + parse (the lexer is pulled by the parser) ── */
    Lexer lexer;
    lexer_init(&lexer, source, cfg.input, arena);
    Parser parser;
    {   /* standard library: $TAMIZHI_LIB, then lib/ next to the compiler */
        char exe[4096], buf[4200];
        parser_add_lib_dir(getenv("TAMIZHI_LIB"));
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) *slash = '\0';
            snprintf(buf, sizeof buf, "%s/lib", exe);
            parser_add_lib_dir(arena_strdup(arena, buf));
            snprintf(buf, sizeof buf, "%s/../lib/tamizhi", exe);
            parser_add_lib_dir(arena_strdup(arena, buf));
        }
        parser_add_lib_dir("/usr/local/lib/tamizhi");
        parser_add_lib_dir("/usr/lib/tamizhi");
    }
    parser_init(&parser, &lexer, arena);
    Node *ast = parse_program(&parser);
    double t_parse = now_ms();

    int rc = 1;
    CodeGen cg;
    bool cg_ready = false;
    char *obj_name = NULL;

    if (g_error_count) {
        fprintf(stderr, "%d error(s) found while parsing; nothing was compiled\n",
                g_error_count);
        goto done;
    }

    /* ── Semantic analysis ── */
    Sema sema;
    sema_init(&sema, arena, cfg.input);
    sema_analyse(&sema, ast);
    if (!g_error_count && !sema.has_main && !cfg.emit_obj && !cfg.emit_c) {
        report_error(cfg.input, 1, 1,
                     "no 'main' function defined (use -c or -S to compile a "
                     "library)");
    }
    double t_sema = now_ms();
    if (g_error_count) {
        fprintf(stderr, "%d error(s)%s found during semantic analysis; nothing "
                        "was compiled\n", g_error_count,
                g_warning_count ? tfmt_warn() : "");
        goto done;
    }
    if (g_warning_count)
        fprintf(stderr, "%d warning(s)\n", g_warning_count);

    /* ── Code generation ── */
    codegen_init(&cg, arena, cfg.input);
    cg_ready = true;
    cg.gc = cfg.gc;
    cg.checks = cfg.checks;
    cg.lines = cfg.lines;
    codegen_generate(&cg, ast);
    double t_gen = now_ms();

    /* ── Back end ── */
    if (cfg.emit_c) {
        FILE *fp = cfg.output ? fopen(cfg.output, "w") : stdout;
        if (!fp) { perror(cfg.output); goto done; }
        codegen_write(&cg, fp);
        if (cfg.output) fclose(fp);
        rc = 0;
    } else {
        const char *out = cfg.output;
        if (!out) {
            if (cfg.emit_obj) out = obj_name = default_object_name(cfg.input);
            else out = "a.out";
        }
        if (!out) { fprintf(stderr, "out of memory\n"); goto done; }
        rc = run_cc(&cfg, out, &cg);
        if (rc != 0)
            fprintf(stderr, "host C compiler '%s' failed (exit %d)\n", cfg.cc, rc);
        if (rc != 0 && cg.gc && cg.uses_heap && !cfg.emit_obj)
            fprintf(stderr, "hint: this program allocates and GC is on, so it "
                            "links the Boehm GC (-lgc).\n      Install libgc "
                            "(Debian/Ubuntu: libgc-dev), pass -L<dir> to its "
                            "location, or compile with --no-gc.\n");
    }

    if (cfg.verbose) {
        double t_end = now_ms();
        fprintf(stderr,
                "[tamizhi] %zu bytes source, %zu KB arena\n"
                "[tamizhi] read+parse %.2f ms | sema %.2f ms | codegen %.2f ms "
                "| cc %.2f ms | total %.2f ms\n",
                strlen(source), arena->total_allocated / 1024,
                t_parse - t0, t_sema - t_parse, t_gen - t_sema,
                t_end - t_gen, t_end - t0);
    }

done:
    if (cg_ready) codegen_free(&cg);
    for (size_t i = 0; i < cfg.inc.n; i++)    free((void *)cfg.inc.v[i]);
    for (size_t i = 0; i < cfg.libdir.n; i++) free((void *)cfg.libdir.v[i]);
    for (size_t i = 0; i < cfg.lib.n; i++)    free((void *)cfg.lib.v[i]);
    free(cfg.inc.v); free(cfg.libdir.v); free(cfg.lib.v); free(cfg.extra.v);
    free(obj_name);
    arena_destroy(arena);
    free(source);
    return rc;
}

/* The tree is walked recursively, so a pathological input (a 200,000-term
 * sum, say) needs a deep stack: run the compiler on a thread with a big one. */
typedef struct { int argc; char **argv; int rc; } MainArgs;
static void *main_thread(void *v) {
    MainArgs *a = v;
    a->rc = real_main(a->argc, a->argv);
    return NULL;
}

int main(int argc, char **argv) {
    MainArgs a = { argc, argv, 0 };
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)1 << 30);      /* 1 GiB, committed lazily */
    if (pthread_create(&th, &at, main_thread, &a) != 0) return real_main(argc, argv);
    pthread_join(th, NULL);
    return a.rc;
}
