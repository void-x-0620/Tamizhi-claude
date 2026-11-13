/*
 * parser.c — Recursive-descent parser (see parser.h for the grammar).
 */
#include "parser.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static Node *parse_decl(Parser *p);
static Node *parse_func_decl(Parser *p, bool is_static);
static Node *parse_struct_decl(Parser *p, bool is_union);
static Node *parse_var_decl(Parser *p, int quals);
static Node *parse_enum_decl(Parser *p);
static Node *parse_typedef_decl(Parser *p);
static Node *parse_class_decl(Parser *p);
static Node *parse_for_stmt(Parser *p);
static Node *parse_do_stmt(Parser *p);
static Node *parse_try_stmt(Parser *p);
static Node *parse_switch_stmt(Parser *p);
static Node *parse_ternary(Parser *p);
static Node *parse_import(Parser *p);
static Node *parse_extern(Parser *p);
#define MAX_NEST 1000
static void nest_abort(Parser *p);

static Node *parse_block(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_if_stmt(Parser *p);
static Node *parse_while_stmt(Parser *p);
static Node *parse_return_stmt(Parser *p);
static Node *parse_pathi_stmt(Parser *p);
static Node *parse_expr(Parser *p);
static Node *parse_assignment(Parser *p);
static Node *parse_binary_level(Parser *p, int level);
static Node *parse_unary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_primary(Parser *p);
static TypeInfo *parse_type(Parser *p);

/* ─── Token helpers ────────────────────────────────────────────────────── */
static Token peek(Parser *p)    { return lexer_peek(p->lexer); }
static void nest_abort(Parser *p) {
    Token t = peek(p);
    report_error(p->lexer->filename, t.line, t.col,
                 "code is nested too deeply (limit %d levels)", MAX_NEST);
    fprintf(stderr, "1 error(s) found; nothing was compiled\n");
    exit(1);
}


static bool  check(Parser *p, TokenType t) { return peek(p).type == t; }

static Token advance(Parser *p) {
    p->consumed++;
    return lexer_next(p->lexer);
}

static bool match(Parser *p, TokenType t) {
    if (check(p, t)) { advance(p); return true; }
    return false;
}

static const char *lexeme(Parser *p, Token t) {
    return arena_strndup(p->arena, t.start, (size_t)t.length);
}

/* Report a syntax error unless we're already recovering from one. */
static void syntax_error(Parser *p, Token at, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void syntax_error(Parser *p, Token at, const char *fmt, ...) {
    if (p->panic) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    report_error(p->lexer->filename, at.line, at.col, "%s", msg);
    p->panic = true;
}

static Token expect(Parser *p, TokenType t) {
    Token tok = peek(p);
    if (tok.type == t) {
        /* Consuming a statement terminator or block close is a natural
         * resynchronisation point, so recovery ends here instead of
         * discarding the following statement as well. */
        if (t == TOK_SEMICOLON || t == TOK_RBRACE) p->panic = false;
        return advance(p);
    }
    syntax_error(p, tok, "expected %s but found %s",
                 token_type_name(t), token_type_name(tok.type));
    return tok;
}

static bool is_qual(TokenType t) {
    return t == TOK_NILAI || t == TOK_THANGU || t == TOK_MAARIDUM;
}

/* Tokens that can only start a top-level declaration: recovery stops here. */
static bool is_decl_start(TokenType t) {
    return t == TOK_SEYAL || t == TOK_KATTAMAIPU || t == TOK_VELI ||
           t == TOK_SERKU || t == TOK_PATTIYAL || t == TOK_INAIPPU ||
           t == TOK_PEYAR || t == TOK_VAGUPU || t == TOK_MOOLAM;
}

/* nilai / thangu / maaridum, in any order, each at most once. */
static int parse_quals(Parser *p) {
    int q = 0;
    while (is_qual(peek(p).type)) {
        Token t = advance(p);
        int bit = t.type == TOK_NILAI ? Q_CONST
                : t.type == TOK_THANGU ? Q_STATIC : Q_VOLATILE;
        if (q & bit) syntax_error(p, t, "duplicate %s", token_type_name(t.type));
        q |= bit;
    }
    return q;
}

/* Skip to the next statement/declaration boundary. */
static void synchronise(Parser *p) {
    p->panic = false;
    while (!check(p, TOK_EOF)) {
        TokenType t = peek(p).type;
        if (t == TOK_SEMICOLON) { advance(p); return; }
        if (t == TOK_RBRACE || is_decl_start(t)) return;
        advance(p);
    }
}

/* ─── Entry point ──────────────────────────────────────────────────────── */
void parser_init(Parser *p, Lexer *L, Arena *arena) {
    p->lexer = L;
    p->arena = arena;
    p->panic = false;
    p->loop_depth = 0;
    p->func_depth = 0;
    p->switch_depth = 0;
    p->consumed = 0;
    p->binding = false;
    p->saw_header = false;
    p->lam_head = p->lam_tail = NULL;
    p->nest = 0;
    ast_current_file = L->filename;
    p->imports = arena_alloc(arena, sizeof(ImportSet));
    memset(p->imports, 0, sizeof *p->imports);
    char real[PATH_MAX];                       /* never import the main file again */
    if (L->filename && realpath(L->filename, real))
        p->imports->paths[p->imports->count++] = arena_strdup(arena, real);
}

Node *parse_program(Parser *p) {
    Node *root = node_new(p->arena, NK_PROGRAM, 1, 1);
    NodeListBuilder b = {0};

    while (!check(p, TOK_EOF)) {
        if (p->panic) {
            synchronise(p);
            if (check(p, TOK_RBRACE)) advance(p);    /* stray '}' at top level */
            continue;
        }
        size_t before = p->consumed;
        Node *d = parse_decl(p);
        if (d && d->kind == NK_PROGRAM) {            /* result of 'serku' */
            for (NodeList *it = d->program.decls; it; it = it->next)
                nlb_push(p->arena, &b, it->node);
        } else if (d) {
            nlb_push(p->arena, &b, d);
        }
        if (p->consumed == before && !check(p, TOK_EOF)) advance(p);
    }
    for (NodeList *it = p->lam_head; it; it = it->next) nlb_push(p->arena, &b, it->node);
    root->program.decls = b.head;
    return root;
}

/* ─── Declarations ─────────────────────────────────────────────────────── */
static Node *parse_decl(Parser *p) {
    Token tok = peek(p);
    if (p->binding && tok.type != TOK_VELI && tok.type != TOK_SERKU) {
        syntax_error(p, tok, "a binding file may only contain 'veli' and "
                             "'serku' declarations, found %s",
                     token_type_name(tok.type));
        return NULL;
    }

    int quals = 0;
    if (is_qual(tok.type)) {
        quals = parse_quals(p);
        tok = peek(p);
        if (tok.type != TOK_EDU && tok.type != TOK_SEYAL) {
            syntax_error(p, tok, "expected 'edu' or 'seyal' after a qualifier "
                                 "but found %s", token_type_name(tok.type));
            return NULL;
        }
    }

    switch (tok.type) {
        case TOK_SERKU:      return parse_import(p);
        case TOK_VELI:       return parse_extern(p);
        case TOK_SEYAL:
            if (quals & ~Q_STATIC)
                syntax_error(p, tok, "only 'thangu' can qualify a function");
            return parse_func_decl(p, (quals & Q_STATIC) != 0);
        case TOK_KATTAMAIPU: return parse_struct_decl(p, false);
        case TOK_INAIPPU:    return parse_struct_decl(p, true);
        case TOK_PATTIYAL:   return parse_enum_decl(p);
        case TOK_PEYAR:      return parse_typedef_decl(p);
        case TOK_VAGUPU: case TOK_MOOLAM: return parse_class_decl(p);
        case TOK_EDU: {
            Node *n = parse_var_decl(p, quals);
            expect(p, TOK_SEMICOLON);
            return n;
        }
        default:
            syntax_error(p, tok,
                "unexpected %s at top level; only 'seyal', 'kattamaipu', "
                "'inaippu', 'pattiyal', 'peyar', 'vagupu', 'edu', 'serku' and "
                "'veli' declarations are allowed here",
                token_type_name(tok.type));
            return NULL;
    }
}

/* ─── C bindings: serku (import) and veli (extern) ──────────────────────── */

/* Directory part of `file` ("" if none), copied into buf. */
static void dir_of(const char *file, char *buf, size_t cap) {
    const char *slash = strrchr(file, '/');
    size_t n = slash ? (size_t)(slash - file) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(buf, file, n);
    buf[n] = '\0';
}

#define MAX_LIB_DIRS 16
static const char *lib_dirs[MAX_LIB_DIRS];
static int n_lib_dirs;

void parser_add_lib_dir(const char *dir) {
    if (dir && *dir && n_lib_dirs < MAX_LIB_DIRS) lib_dirs[n_lib_dirs++] = dir;
}

/* Look for `name` next to the including file first, then in the current
 * directory. Writes the path that worked (or the first guess) into out. */
static char *open_relative(Parser *p, const char *name, char *out, size_t cap) {
    char dir[PATH_MAX];
    dir_of(p->lexer->filename, dir, sizeof dir);
    if (name[0] != '/' && dir[0]) {
        int n = snprintf(out, cap, "%s/%s", dir, name);
        if (n > 0 && (size_t)n < cap) {
            char *src = read_source_file(out, p->arena);
            if (src) return src;
        }
    }
    snprintf(out, cap, "%s", name);
    char *src = read_source_file(out, p->arena);
    if (src || name[0] == '/') return src;
    for (int i = 0; i < n_lib_dirs; i++) {          /* standard library dirs */
        int n = snprintf(out, cap, "%s/%s", lib_dirs[i], name);
        if (n > 0 && (size_t)n < cap && (src = read_source_file(out, p->arena)))
            return src;
    }
    snprintf(out, cap, "%s", name);
    return NULL;
}

/* serku "file.tzb";  — parse another file's declarations in place. */
static Node *parse_import(Parser *p) {
    Token kw   = expect(p, TOK_SERKU);
    Token path = peek(p);
    if (path.type != TOK_STRING_LIT) {
        syntax_error(p, path, "expected a file name in quotes after 'serku' "
                              "but found %s", token_type_name(path.type));
        return NULL;
    }
    advance(p);
    expect(p, TOK_SEMICOLON);
    if (p->panic) return NULL;

    const char *file = p->lexer->filename;
    char cand[PATH_MAX];
    char *src = open_relative(p, path.val.str_val, cand, sizeof cand);
    if (!src) {
        report_error(file, kw.line, kw.col, "cannot open '%s': %s",
                     path.val.str_val, strerror(errno));
        return NULL;
    }

    ImportSet *imp = p->imports;
    char real[PATH_MAX];
    const char *key = realpath(cand, real) ? real : cand;
    for (int i = 0; i < imp->count; i++)
        if (!strcmp(imp->paths[i], key)) return NULL;     /* include once */
    if (imp->count >= MAX_IMPORTS) {
        report_error(file, kw.line, kw.col, "too many imported files (max %d)",
                     MAX_IMPORTS);
        return NULL;
    }
    if (imp->depth >= MAX_IMPORT_DEPTH) {
        report_error(file, kw.line, kw.col, "imports are nested too deeply");
        return NULL;
    }
    imp->paths[imp->count++] = arena_strdup(p->arena, key);

    Lexer sub;
    lexer_init(&sub, src, arena_strdup(p->arena, cand), p->arena);
    Parser sp;
    const char *saved_file = ast_current_file;
    parser_init(&sp, &sub, p->arena);
    sp.imports = imp;                       /* share the include-once set */
    /* .tzb = C binding file (veli / serku only); anything else, conventionally
     * .tzh, is a Tamizhi header: ordinary declarations, no restrictions. */
    size_t cl = strlen(cand);
    sp.binding = cl >= 4 && !strcmp(cand + cl - 4, ".tzb");
    imp->depth++;
    Node *prog = parse_program(&sp);
    imp->depth--;
    ast_current_file = saved_file;
    return prog;
}

/* Text to put after #include: a header sitting next to the binding file
 * wins (by absolute path); otherwise the system/-I search path is used. */
static const char *header_spec(Parser *p, const char *name) {
    size_t len = strlen(name);
    char *spec;
    if (name[0] == '<') {                                  /* already <x.h> */
        spec = arena_strdup(p->arena, name);
    } else {
        char cand[PATH_MAX], real[PATH_MAX];
        char dir[PATH_MAX];
        dir_of(p->lexer->filename, dir, sizeof dir);
        int n = (name[0] == '/' || !dir[0]) ? snprintf(cand, sizeof cand, "%s", name)
                                            : snprintf(cand, sizeof cand, "%s/%s", dir, name);
        if (n > 0 && (size_t)n < sizeof cand && realpath(cand, real) &&
            !strpbrk(real, "\"\\\n")) {
            spec = arena_alloc(p->arena, strlen(real) + 3);
            sprintf(spec, "\"%s\"", real);
        } else if (strpbrk(name, "\"\\\n>")) {
            spec = arena_strdup(p->arena, "<invalid.h>");
        } else {
            spec = arena_alloc(p->arena, len + 3);
            sprintf(spec, "<%s>", name);
        }
    }
    return spec;
}

/* Only these types can cross the C boundary: numbers, bool, string, void,
 * and pointers / function pointers built from them. */
static bool c_type_ok(TypeInfo *t, bool allow_void) {
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_STRING: case TY_BOOL: return true;
        case TY_VOID:    return allow_void;
        case TY_POINTER: return c_type_ok(t->elem_type, true);
        case TY_FUNC: {
            for (Param *q = t->fparams; q; q = q->next)
                if (!c_type_ok(q->type, false)) return false;
            return c_type_ok(t->fret, true);
        }
        default: return false;
    }
}

static void check_c_type(Parser *p, TypeInfo *t, Token at, bool allow_void) {
    if (t == &ty_unknown) return;                      /* already reported */
    if (c_type_ok(t, allow_void)) return;
    report_error(p->lexer->filename, at.line, at.col,
                 "a C binding can only use numeric types, string, bool and "
                 "pointers%s", allow_void ? " (or no return type)" : "");
}

/* veli "header.h";
 * veli seyal name [= c_name] ( p : type, ... ) [: type] ;                   */
static Node *parse_extern(Parser *p) {
    Token kw = expect(p, TOK_VELI);

    if (check(p, TOK_STRING_LIT)) {
        Token h = advance(p);
        expect(p, TOK_SEMICOLON);
        if (!h.val.str_val[0]) {
            report_error(p->lexer->filename, h.line, h.col, "empty header name");
            return NULL;
        }
        Node *n = node_new(p->arena, NK_EXTERN_HEADER, kw.line, kw.col);
        n->extern_header.spec = header_spec(p, h.val.str_val);
        p->saw_header = true;
        return n;
    }

    expect(p, TOK_SEYAL);
    Token name  = expect(p, TOK_IDENT);
    Token cname = name;
    if (match(p, TOK_EQ)) cname = expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);

    Param *head = NULL, *tail = NULL;
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Token pn = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        Token tt = peek(p);
        TypeInfo *pt = parse_type(p);
        check_c_type(p, pt, tt, false);
        Param *par = param_new(p->arena, lexeme(p, pn), pt);
        if (tail) tail->next = par; else head = par;
        tail = par;
        if (p->panic || !match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);

    TypeInfo *ret = NULL;
    if (match(p, TOK_COLON)) {
        Token rt = peek(p);
        ret = parse_type(p);
        check_c_type(p, ret, rt, true);
    }
    expect(p, TOK_SEMICOLON);

    Node *n = node_new(p->arena, NK_EXTERN_FUNC, kw.line, kw.col);
    n->extern_func.name       = lexeme(p, name);
    n->extern_func.cname      = lexeme(p, cname);
    n->extern_func.params     = head;
    n->extern_func.ret_type   = ret;
    n->extern_func.has_header = p->saw_header;
    n->extern_func.file       = p->lexer->filename;
    return n;
}

static const char *lambda_name_pending;
static Node *parse_func_decl(Parser *p, bool is_static) {
    Token kw   = expect(p, TOK_SEYAL);
    Token name = {0};
    const char *fname;
    if (lambda_name_pending) { fname = lambda_name_pending; lambda_name_pending = NULL; }
    else { name = expect(p, TOK_IDENT); fname = lexeme(p, name); }
    expect(p, TOK_LPAREN);

    Param *head = NULL, *tail = NULL;
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Token pn = expect(p, TOK_IDENT);
        TypeInfo *pt = NULL;
        if (match(p, TOK_COLON)) pt = parse_type(p);
        Param *par = param_new(p->arena, lexeme(p, pn), pt);
        if (tail) tail->next = par; else head = par;
        tail = par;
        if (p->panic || !match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);

    TypeInfo *ret = NULL;
    if (match(p, TOK_COLON)) ret = parse_type(p);

    p->func_depth++;
    Node *body = parse_block(p);
    p->func_depth--;

    Node *n = node_new(p->arena, NK_FUNC_DECL, kw.line, kw.col);
    n->func_decl.name     = fname;
    n->func_decl.params   = head;
    n->func_decl.ret_type = ret;
    n->func_decl.body     = body;
    n->func_decl.is_static = is_static;
    return n;
}

static Node *parse_struct_decl(Parser *p, bool is_union) {
    Token kw   = expect(p, is_union ? TOK_INAIPPU : TOK_KATTAMAIPU);
    Token name = expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    Field *head = NULL, *tail = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->panic) {
        Token fn = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        TypeInfo *ft = parse_type(p);
        expect(p, TOK_SEMICOLON);
        Field *f = field_new(p->arena, lexeme(p, fn), ft);
        if (tail) tail->next = f; else head = f;
        tail = f;
    }
    expect(p, TOK_RBRACE);

    Node *n = node_new(p->arena, NK_STRUCT_DECL, kw.line, kw.col);
    n->struct_decl.name   = lexeme(p, name);
    n->struct_decl.fields = head;
    n->struct_decl.is_union = is_union;
    return n;
}

static Node *parse_var_decl(Parser *p, int quals) {
    expect(p, TOK_EDU);
    Token name = expect(p, TOK_IDENT);

    TypeInfo *vt = NULL;
    if (match(p, TOK_COLON)) vt = parse_type(p);

    Node *init = NULL;
    if (match(p, TOK_EQ)) init = parse_expr(p);

    Node *n = node_new(p->arena, NK_VAR_DECL, name.line, name.col);
    n->var_decl.name     = lexeme(p, name);
    n->var_decl.var_type = vt;
    n->var_decl.init     = init;
    n->var_decl.quals    = quals;
    return n;
}

/* type ::= (IDENT | 'seyal' '(' [type (',' type)*] ')' [':' type])
 *           ('*' | '[' [INT] ']')*                                           */
/* List<T>, Set<T> and Map<K,V> (the name token is already consumed). */
static void close_angle(Parser *p) {
    if (check(p, TOK_SHR)) {                /* List<List<int>>: split '>>' */
        p->lexer->peeked.type = TOK_GT;
        p->lexer->peeked.start++;
        p->lexer->peeked.length--;
        p->lexer->peeked.col++;
    } else {
        expect(p, TOK_GT);
    }
}

static TypeInfo *parse_collection_type(Parser *p, Token name) {
    if (name.type != TOK_IDENT || !check(p, TOK_LT)) return NULL;
    if (name.length == 2 && !memcmp(name.start, "Fn", 2)) {      /* Fn<(A, B): R> */
        advance(p);
        expect(p, TOK_LPAREN);
        Param *head = NULL, *tail = NULL;
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            Param *par = param_new(p->arena, "_", parse_type(p));
            if (tail) tail->next = par; else head = par;
            tail = par;
            if (p->panic || !match(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RPAREN);
        TypeInfo *ret = &ty_void;
        if (match(p, TOK_COLON)) ret = parse_type(p);
        close_angle(p);
        TypeInfo *t = type_func(p->arena, head, ret);
        t->kind = TY_CLOSURE;
        return t;
    }
    bool list = name.length == 4 && !memcmp(name.start, "List", 4);
    bool set  = name.length == 3 && !memcmp(name.start, "Set", 3);
    bool map  = name.length == 3 && !memcmp(name.start, "Map", 3);
    if (!list && !set && !map) return NULL;
    advance(p);
    TypeInfo *first = parse_type(p), *t;
    if (map) {
        expect(p, TOK_COMMA);
        TypeInfo *val = parse_type(p);
        t = type_new(p->arena, TY_MAP);
        t->key_type = first;
        t->elem_type = val;
    } else if (set) {
        t = type_new(p->arena, TY_MAP);
        t->key_type = first;
        t->elem_type = &ty_bool;
        t->is_set = true;
    } else {
        t = type_new(p->arena, TY_LIST);
        t->elem_type = first;
    }
    close_angle(p);
    return t;
}

static TypeInfo *parse_type(Parser *p) {
    Token tok = peek(p);
    TypeInfo *base;

    if (tok.type == TOK_SEYAL) {                       /* function pointer */
        advance(p);
        expect(p, TOK_LPAREN);
        Param *head = NULL, *tail = NULL;
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            TypeInfo *pt = parse_type(p);
            Param *par = param_new(p->arena, "_", pt);
            if (tail) tail->next = par; else head = par;
            tail = par;
            if (p->panic || !match(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RPAREN);
        TypeInfo *ret = &ty_void;
        if (match(p, TOK_COLON)) ret = parse_type(p);
        base = type_func(p->arena, head, ret);
    } else if (tok.type != TOK_IDENT) {
        syntax_error(p, tok, "expected a type name but found %s",
                     token_type_name(tok.type));
        return &ty_unknown;
    } else {
        advance(p);
        base = builtin_type(tok.start, tok.length);
        TypeInfo *coll = base ? NULL : parse_collection_type(p, tok);
        if (coll) {
            base = coll;
        } else if (!base) {                /* struct, enum, class or alias */
            base = type_new(p->arena, TY_STRUCT);
            base->struct_name = lexeme(p, tok);
        }
    }

    /* int[2][3] is an array of 2 arrays of 3 ints (like C): the first
     * bracket is the outermost, so later ones are pushed inwards. */
    TypeInfo *arr_tail = NULL;
    for (;;) {
        if (check(p, TOK_STAR)) {
            advance(p);
            base = type_pointer(p->arena, base);
            arr_tail = NULL;
        } else if (check(p, TOK_LBRACKET)) {
            advance(p);
            int size = -1;
            if (check(p, TOK_INT_LIT)) {
                Token n = advance(p);
                if (n.val.int_val <= 0 || n.val.int_val > (1 << 28)) {
                    syntax_error(p, n, "array size must be between 1 and %d", 1 << 28);
                } else {
                    size = (int)n.val.int_val;
                }
            }
            expect(p, TOK_RBRACKET);
            if (arr_tail) {
                TypeInfo *inner = type_array(p->arena, arr_tail->elem_type, size);
                arr_tail->elem_type = inner;
                arr_tail = inner;
            } else {
                base = type_array(p->arena, base, size);
                arr_tail = base;
            }
        } else {
            break;
        }
    }
    return base;
}

/* pattiyal Name { A, B = 5, C } */
static Node *parse_enum_decl(Parser *p) {
    Token kw   = expect(p, TOK_PATTIYAL);
    Token name = expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);
    Enumerator *head = NULL, *tail = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->panic) {
        Token en = expect(p, TOK_IDENT);
        Enumerator *e = arena_alloc(p->arena, sizeof *e);
        memset(e, 0, sizeof *e);
        e->name = lexeme(p, en);
        e->line = en.line; e->col = en.col;
        if (match(p, TOK_EQ)) e->value = parse_ternary(p);
        if (tail) tail->next = e; else head = e;
        tail = e;
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RBRACE);
    Node *n = node_new(p->arena, NK_ENUM_DECL, kw.line, kw.col);
    n->enum_decl.name  = lexeme(p, name);
    n->enum_decl.items = head;
    return n;
}

/* peyar Name = type ; */
static Node *parse_typedef_decl(Parser *p) {
    Token kw   = expect(p, TOK_PEYAR);
    Token name = expect(p, TOK_IDENT);
    expect(p, TOK_EQ);
    TypeInfo *t = parse_type(p);
    expect(p, TOK_SEMICOLON);
    Node *n = node_new(p->arena, NK_TYPEDEF_DECL, kw.line, kw.col);
    n->typedef_decl.name   = lexeme(p, name);
    n->typedef_decl.target = t;
    return n;
}


/* vagupu Name [':' Parent] '{' member* '}'
 *   member ::= [access] ['thangu'] IDENT ':' type ['=' expr] ';'     field
 *            | [access] ['thangu'] 'seyal' IDENT '(' params ')' [':' type] block
 *            | [access] 'uruvaakku' '(' params ')' block             constructor
 *   access ::= 'pothu' | 'paathukaappu' | 'maraivu'    (default: pothu)      */
static Param *parse_params(Parser *p) {
    Param *head = NULL, *tail = NULL;
    expect(p, TOK_LPAREN);
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Token pn = expect(p, TOK_IDENT);
        TypeInfo *pt = NULL;
        if (match(p, TOK_COLON)) pt = parse_type(p);
        Param *par = param_new(p->arena, lexeme(p, pn), pt);
        if (tail) tail->next = par; else head = par;
        tail = par;
        if (p->panic || !match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);
    return head;
}

static Node *parse_class_decl(Parser *p) {
    bool is_abs = match(p, TOK_MOOLAM);
    Token kw   = expect(p, TOK_VAGUPU);
    Token name = expect(p, TOK_IDENT);
    const char *parent = NULL;
    if (match(p, TOK_COLON)) parent = lexeme(p, expect(p, TOK_IDENT));
    expect(p, TOK_LBRACE);

    Field  *fh = NULL, *ft = NULL;
    Method *mh = NULL, *mt = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->panic) {
        Access acc = ACC_PUBLIC;
        Token t = peek(p);
        if (t.type == TOK_POTHU || t.type == TOK_MARAIVU ||
            t.type == TOK_PAATHUKAAPPU) {
            advance(p);
            acc = t.type == TOK_POTHU ? ACC_PUBLIC
                : t.type == TOK_MARAIVU ? ACC_PRIVATE : ACC_PROTECTED;
        }
        bool is_static = false, is_abs_m = false;
        for (;;) {
            if (check(p, TOK_THANGU)) { advance(p); is_static = true; }
            else if (check(p, TOK_MOOLAM)) { advance(p); is_abs_m = true; }
            else break;
        }
        t = peek(p);

        if (t.type == TOK_URUVAAKKU) {
            advance(p);
            if (is_static) syntax_error(p, t, "a constructor cannot be 'thangu'");
            Method *m = arena_alloc(p->arena, sizeof *m);
            memset(m, 0, sizeof *m);
            m->name = lexeme(p, name);
            m->is_ctor = true; m->access = acc; m->slot = -1;
            m->line = t.line; m->col = t.col;
            m->params = parse_params(p);
            p->func_depth++;
            m->body = parse_block(p);
            p->func_depth--;
            if (mt) mt->next = m; else mh = m;
            mt = m;
        } else if (t.type == TOK_SEYAL) {
            advance(p);
            Token mn = expect(p, TOK_IDENT);
            Method *m = arena_alloc(p->arena, sizeof *m);
            memset(m, 0, sizeof *m);
            m->name = lexeme(p, mn);
            m->is_static = is_static; m->access = acc; m->slot = -1;
            m->line = mn.line; m->col = mn.col;
            m->params = parse_params(p);
            if (match(p, TOK_COLON)) m->ret_type = parse_type(p);
            if (is_abs_m) {
                m->is_abstract = true;
                if (is_static)
                    report_error(p->lexer->filename, mn.line, mn.col,
                                 "a 'moolam' (abstract) method cannot be 'thangu'");
                expect(p, TOK_SEMICOLON);
            } else {
                p->func_depth++;
                m->body = parse_block(p);
                p->func_depth--;
            }
            if (mt) mt->next = m; else mh = m;
            mt = m;
        } else if (t.type == TOK_IDENT) {
            Token fn = advance(p);
            expect(p, TOK_COLON);
            TypeInfo *fty = parse_type(p);
            Field *f = field_new(p->arena, lexeme(p, fn), fty);
            f->access = acc; f->is_static = is_static;
            f->line = fn.line; f->col = fn.col;
            if (match(p, TOK_EQ)) {
                f->init = parse_expr(p);
                if (!is_static)
                    report_error(p->lexer->filename, fn.line, fn.col,
                                 "only a 'thangu' (static) field can have an "
                                 "initialiser; set '%s' in the constructor",
                                 f->name);
            }
            expect(p, TOK_SEMICOLON);
            if (ft) ft->next = f; else fh = f;
            ft = f;
        } else {
            syntax_error(p, t, "expected a field, 'seyal' or 'uruvaakku' inside "
                               "a class but found %s", token_type_name(t.type));
        }
    }
    expect(p, TOK_RBRACE);

    Node *n = node_new(p->arena, NK_CLASS_DECL, kw.line, kw.col);
    n->class_decl.name    = lexeme(p, name);
    n->class_decl.parent  = parent;
    n->class_decl.is_abstract = is_abs;
    n->class_decl.fields  = fh;
    n->class_decl.methods = mh;
    return n;
}

/* ─── Statements ───────────────────────────────────────────────────────── */
static Node *parse_block_inner(Parser *p);
static Node *parse_block(Parser *p) {
    if (++p->nest > MAX_NEST) nest_abort(p);
    Node *r = parse_block_inner(p);
    p->nest--;
    return r;
}
static Node *parse_block_inner(Parser *p) {
    Token lb = expect(p, TOK_LBRACE);
    Node *blk = node_new(p->arena, NK_BLOCK, lb.line, lb.col);
    NodeListBuilder b = {0};

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (p->panic) {
            synchronise(p);
            /* A new top-level declaration means this block lost its '}'. */
            if (is_decl_start(peek(p).type)) break;
            continue;
        }
        size_t before = p->consumed;
        Node *s = parse_stmt(p);
        if (s) nlb_push(p->arena, &b, s);
        if (p->consumed == before && !check(p, TOK_RBRACE) &&
            !check(p, TOK_EOF))
            advance(p);
    }
    expect(p, TOK_RBRACE);
    blk->block.stmts = b.head;
    return blk;
}

static Node *parse_stmt_inner(Parser *p);
static Node *parse_stmt(Parser *p) {
    if (++p->nest > MAX_NEST) nest_abort(p);
    Node *r = parse_stmt_inner(p);
    p->nest--;
    return r;
}
static Node *parse_stmt_inner(Parser *p) {
    Token tok = peek(p);
    switch (tok.type) {
        case TOK_EDU: {
            Node *n = parse_var_decl(p, 0);
            expect(p, TOK_SEMICOLON);
            return n;
        }
        case TOK_NILAI: case TOK_THANGU: case TOK_MAARIDUM: {
            int q = parse_quals(p);
            if (!check(p, TOK_EDU)) {
                syntax_error(p, peek(p), "expected 'edu' after a qualifier but "
                             "found %s", token_type_name(peek(p).type));
                return NULL;
            }
            Node *n = parse_var_decl(p, q);
            expect(p, TOK_SEMICOLON);
            return n;
        }
        case TOK_OVVORU: return parse_for_stmt(p);
        case TOK_SEITHU: return parse_do_stmt(p);
        case TOK_THERVU: return parse_switch_stmt(p);
        case TOK_MUYARCHI: return parse_try_stmt(p);
        case TOK_ERIVI: {
            Token kw = advance(p);
            Node *n = node_new(p->arena, NK_THROW, kw.line, kw.col);
            n->throw_stmt.value = parse_expr(p);
            expect(p, TOK_SEMICOLON);
            return n;
        }
        case TOK_LBRACE: return parse_block(p);          /* nested scope */
        case TOK_AT:    return parse_if_stmt(p);
        case TOK_SUTRI: return parse_while_stmt(p);
        case TOK_KUDU:  return parse_return_stmt(p);
        case TOK_PATHI: return parse_pathi_stmt(p);
        case TOK_UDAI:
        case TOK_THODAR: {
            Token t = advance(p);
            bool brk = t.type == TOK_UDAI;
            if (p->loop_depth == 0 && !(brk && p->switch_depth > 0))
                report_error(p->lexer->filename, t.line, t.col,
                             "'%s' (%s) used outside of a loop%s",
                             brk ? "udai" : "thodar",
                             brk ? "break" : "continue",
                             brk ? " or 'thervu'" : "");
            expect(p, TOK_SEMICOLON);
            return node_new(p->arena, brk ? NK_BREAK : NK_CONTINUE,
                            t.line, t.col);
        }
        default: {
            Node *e = parse_expr(p);
            expect(p, TOK_SEMICOLON);
            Node *n = node_new(p->arena, NK_EXPR_STMT, tok.line, tok.col);
            n->expr_stmt.expr = e;
            return n;
        }
    }
}

/* One "(cond) aga irundha { ... }" arm. */
static IfBranch *parse_if_arm(Parser *p) {
    expect(p, TOK_LPAREN);
    Node *cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    expect(p, TOK_AGA);
    expect(p, TOK_IRUNDHA);
    Node *body = parse_block(p);

    IfBranch *br = arena_alloc(p->arena, sizeof(IfBranch));
    br->cond = cond; br->body = body; br->next = NULL;
    return br;
}

static Node *parse_if_stmt(Parser *p) {
    Token at = expect(p, TOK_AT);
    Node *n = node_new(p->arena, NK_IF, at.line, at.col);

    IfBranch *head = parse_if_arm(p), *tail = head;
    while (check(p, TOK_CARET)) {
        advance(p);
        IfBranch *br = parse_if_arm(p);
        tail->next = br; tail = br;
    }
    if (match(p, TOK_ILLANA)) {
        IfBranch *br = arena_alloc(p->arena, sizeof(IfBranch));
        br->cond = NULL; br->body = parse_block(p); br->next = NULL;
        tail->next = br;
    }
    n->if_stmt.branches = head;
    return n;
}

/* ovvoru ( [edu x = e | expr] ; [cond] ; [update] ) { ... } */
/* ovvoru (edu i ulle a .. b) { body }  ==>  { edu _endN = b; ovvoru (edu i = a; i < _endN; i++) body } */
static Node *desugar_range(Parser *p, Token kw, Node *init, Node *a, Node *b) {
    static int range_count = 0;
    char nm[32];
    snprintf(nm, sizeof nm, "_end%d", ++range_count);
    const char *end_name = arena_strdup(p->arena, nm);
    const char *var = init->var_decl.name;

    Node *end = node_new(p->arena, NK_VAR_DECL, kw.line, kw.col);
    end->var_decl.name = end_name;
    end->var_decl.init = b;

    Node *iv = node_new(p->arena, NK_VAR_DECL, init->line, init->col);
    iv->var_decl.name = var;
    iv->var_decl.var_type = init->var_decl.var_type;
    iv->var_decl.init = a;

    Node *l = node_new(p->arena, NK_IDENT, kw.line, kw.col);  l->ident.name = var;
    Node *r = node_new(p->arena, NK_IDENT, kw.line, kw.col);  r->ident.name = end_name;
    Node *cond = node_new(p->arena, NK_BINARY, kw.line, kw.col);
    cond->binary.op = BOP_LT; cond->binary.left = l; cond->binary.right = r;
    Node *u = node_new(p->arena, NK_IDENT, kw.line, kw.col);  u->ident.name = var;
    Node *inc = node_new(p->arena, NK_INCDEC, kw.line, kw.col);
    inc->incdec.operand = u; inc->incdec.is_inc = true; inc->incdec.prefix = false;

    p->loop_depth++;
    Node *body = parse_block(p);
    p->loop_depth--;

    Node *f = node_new(p->arena, NK_FOR, kw.line, kw.col);
    f->for_stmt.init = iv; f->for_stmt.cond = cond; f->for_stmt.update = inc; f->for_stmt.body = body;

    Node *blk = node_new(p->arena, NK_BLOCK, kw.line, kw.col);
    NodeList *c2 = arena_alloc(p->arena, sizeof *c2), *c1 = arena_alloc(p->arena, sizeof *c1);
    c2->node = f; c2->next = NULL; c1->node = end; c1->next = c2;
    blk->block.stmts = c1;
    return blk;
}

static Node *parse_for_stmt(Parser *p) {
    Token kw = expect(p, TOK_OVVORU);
    expect(p, TOK_LPAREN);
    Node *init = NULL, *cond = NULL, *update = NULL;

    if (!check(p, TOK_SEMICOLON)) {
        Token t = peek(p);
        if (t.type == TOK_EDU || is_qual(t.type)) {
            int q = is_qual(t.type) ? parse_quals(p) : 0;
            init = parse_var_decl(p, q);
            if (check(p, TOK_ULLE) && init && !p->panic) {        /* foreach */
                advance(p);
                Node *fe = node_new(p->arena, NK_FOREACH, kw.line, kw.col);
                fe->foreach_stmt.name  = init->var_decl.name;
                fe->foreach_stmt.vtype = init->var_decl.var_type;
                Node *first = parse_expr(p);
                if (match(p, TOK_DOTDOT)) {          /* range: ovvoru (edu i ulle a .. b) */
                    Node *last = parse_expr(p);
                    expect(p, TOK_RPAREN);
                    return desugar_range(p, kw, init, first, last);
                }
                fe->foreach_stmt.coll  = first;
                expect(p, TOK_RPAREN);
                p->loop_depth++;
                fe->foreach_stmt.body = parse_block(p);
                p->loop_depth--;
                return fe;
            }
        } else {
            Node *e = parse_expr(p);
            init = node_new(p->arena, NK_EXPR_STMT, t.line, t.col);
            init->expr_stmt.expr = e;
        }
    }
    expect(p, TOK_SEMICOLON);
    if (!check(p, TOK_SEMICOLON)) cond = parse_expr(p);
    expect(p, TOK_SEMICOLON);
    if (!check(p, TOK_RPAREN)) update = parse_expr(p);
    expect(p, TOK_RPAREN);

    p->loop_depth++;
    Node *body = parse_block(p);
    p->loop_depth--;

    Node *n = node_new(p->arena, NK_FOR, kw.line, kw.col);
    n->for_stmt.init = init;
    n->for_stmt.cond = cond;
    n->for_stmt.update = update;
    n->for_stmt.body = body;
    return n;
}

/* seithu { ... } sutri (cond) ;   — body runs at least once */
/* muyarchi { ... } pidi (name) { ... }     (the catch variable is a string) */
static Node *parse_try_stmt(Parser *p) {
    Token kw = expect(p, TOK_MUYARCHI);
    Node *n = node_new(p->arena, NK_TRY, kw.line, kw.col);
    n->try_stmt.body = parse_block(p);
    Catch *tail = NULL;
    while (match(p, TOK_PIDI)) {                  /* pidi (e: Type) { } ... */
        Catch *c = arena_alloc(p->arena, sizeof *c);
        memset(c, 0, sizeof *c);
        if (match(p, TOK_LPAREN)) {
            c->name = lexeme(p, expect(p, TOK_IDENT));
            if (match(p, TOK_COLON)) c->type = parse_type(p);
            expect(p, TOK_RPAREN);
        }
        c->body = parse_block(p);
        if (tail) tail->next = c; else n->try_stmt.catches = c;
        tail = c;
    }
    if (match(p, TOK_IRUDHI)) n->try_stmt.finally_body = parse_block(p);
    if (!n->try_stmt.catches && !n->try_stmt.finally_body) {
        Token t = peek(p);
        report_error(p->lexer->filename, t.line, t.col,
                     "'muyarchi' needs a 'pidi' (catch) or an 'irudhi' (finally) block");
    }
    return n;
}

static Node *parse_do_stmt(Parser *p) {
    Token kw = expect(p, TOK_SEITHU);
    p->loop_depth++;
    Node *body = parse_block(p);
    p->loop_depth--;
    expect(p, TOK_SUTRI);
    expect(p, TOK_LPAREN);
    Node *cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    expect(p, TOK_SEMICOLON);
    Node *n = node_new(p->arena, NK_DO_WHILE, kw.line, kw.col);
    n->while_stmt.cond = cond;
    n->while_stmt.body = body;
    return n;
}

/* thervu (e) { vazhakku 1, 2: { ... }  iyalbu: { ... } }
 * No fall-through: each arm ends the switch. udai leaves the switch early.  */
static Node *parse_switch_stmt(Parser *p) {
    Token kw = expect(p, TOK_THERVU);
    expect(p, TOK_LPAREN);
    Node *subject = parse_expr(p);
    expect(p, TOK_RPAREN);
    expect(p, TOK_LBRACE);

    p->switch_depth++;
    SwitchCase *head = NULL, *tail = NULL;
    bool saw_default = false;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->panic) {
        Token t = peek(p);
        SwitchCase *c = arena_alloc(p->arena, sizeof *c);
        memset(c, 0, sizeof *c);
        c->line = t.line; c->col = t.col;
        if (t.type == TOK_VAZHAKKU) {
            advance(p);
            NodeListBuilder b = {0};
            nlb_push(p->arena, &b, parse_ternary(p));
            while (match(p, TOK_COMMA)) nlb_push(p->arena, &b, parse_ternary(p));
            c->labels = b.head;
        } else if (t.type == TOK_IYALBU) {
            advance(p);
            if (saw_default) syntax_error(p, t, "'thervu' already has an 'iyalbu' arm");
            c->is_default = true;
            saw_default = true;
        } else {
            syntax_error(p, t, "expected 'vazhakku' or 'iyalbu' inside 'thervu' "
                               "but found %s", token_type_name(t.type));
            break;
        }
        expect(p, TOK_COLON);
        c->body = parse_block(p);
        if (tail) tail->next = c; else head = c;
        tail = c;
    }
    p->switch_depth--;
    expect(p, TOK_RBRACE);

    Node *n = node_new(p->arena, NK_SWITCH, kw.line, kw.col);
    n->switch_stmt.subject = subject;
    n->switch_stmt.cases = head;
    return n;
}

static Node *parse_while_stmt(Parser *p) {
    Token kw = expect(p, TOK_SUTRI);
    expect(p, TOK_LPAREN);
    Node *cond = parse_expr(p);
    expect(p, TOK_RPAREN);

    p->loop_depth++;
    Node *body = parse_block(p);
    p->loop_depth--;

    Node *n = node_new(p->arena, NK_WHILE, kw.line, kw.col);
    n->while_stmt.cond = cond;
    n->while_stmt.body = body;
    return n;
}

static Node *parse_return_stmt(Parser *p) {
    Token kw = expect(p, TOK_KUDU);
    Node *val = NULL;
    if (!check(p, TOK_SEMICOLON)) val = parse_expr(p);
    expect(p, TOK_SEMICOLON);
    Node *n = node_new(p->arena, NK_RETURN, kw.line, kw.col);
    n->ret.value = val;
    return n;
}

static Node *parse_pathi_stmt(Parser *p) {
    Token kw = expect(p, TOK_PATHI);
    expect(p, TOK_LPAREN);
    Node *fmt = parse_expr(p);
    NodeListBuilder b = {0};
    while (match(p, TOK_COMMA))
        nlb_push(p->arena, &b, parse_expr(p));
    expect(p, TOK_RPAREN);
    expect(p, TOK_SEMICOLON);

    Node *n = node_new(p->arena, NK_PATHI, kw.line, kw.col);
    n->pathi.fmt  = fmt;
    n->pathi.args = b.head;
    return n;
}

/* ─── Expressions ──────────────────────────────────────────────────────── */
static Node *parse_expr(Parser *p) { return parse_assignment(p); }

static const struct { TokenType tok; int op; } ASSIGN_OPS[] = {
    { TOK_EQ, BOP_NONE },     { TOK_PLUS_EQ, BOP_ADD },  { TOK_MINUS_EQ, BOP_SUB },
    { TOK_STAR_EQ, BOP_MUL }, { TOK_SLASH_EQ, BOP_DIV }, { TOK_PERCENT_EQ, BOP_MOD },
    { TOK_AMP_EQ, BOP_BAND }, { TOK_PIPE_EQ, BOP_BOR },  { TOK_CARET_EQ, BOP_BXOR },
    { TOK_SHL_EQ, BOP_SHL },  { TOK_SHR_EQ, BOP_SHR },
};

/* assignment ::= ternary [ ('=' | '+=' | '-=' | ...) assignment ]   (right assoc) */
static Node *parse_assignment(Parser *p) {
    Node *left = parse_ternary(p);
    TokenType t = peek(p).type;
    for (size_t i = 0; i < sizeof ASSIGN_OPS / sizeof ASSIGN_OPS[0]; i++) {
        if (ASSIGN_OPS[i].tok != t) continue;
        Token eq = advance(p);
        Node *val = parse_assignment(p);
        bool ok = left->kind == NK_IDENT || left->kind == NK_INDEX ||
                  left->kind == NK_MEMBER ||
                  (left->kind == NK_UNARY && left->unary.op == UOP_DEREF);
        if (!ok) syntax_error(p, eq, "invalid assignment target");
        Node *n = node_new(p->arena, NK_ASSIGN, eq.line, eq.col);
        n->assign.target = left;
        n->assign.value  = val;
        n->assign.op     = ASSIGN_OPS[i].op;
        return n;
    }
    return left;
}

/* ternary ::= binary [ '?' assignment ':' ternary ] */
static Node *parse_ternary(Parser *p) {
    Node *cond = parse_binary_level(p, 0);
    if (!check(p, TOK_QUESTION)) return cond;
    Token q = advance(p);
    Node *a = parse_assignment(p);
    expect(p, TOK_COLON);
    Node *b = parse_ternary(p);
    Node *n = node_new(p->arena, NK_TERNARY, q.line, q.col);
    n->ternary.cond = cond;
    n->ternary.then_e = a;
    n->ternary.else_e = b;
    return n;
}

/* Left-associative binary levels, loosest first (the same order as C). */
typedef struct { TokenType tok; BinOp op; } BinTok;
static const BinTok LV_OR[]  = { { TOK_PIPE_PIPE, BOP_OR  }, { TOK_EOF, 0 } };
static const BinTok LV_AND[] = { { TOK_AMP_AMP,   BOP_AND }, { TOK_EOF, 0 } };
static const BinTok LV_BOR[] = { { TOK_PIPE,  BOP_BOR  }, { TOK_EOF, 0 } };
static const BinTok LV_BXOR[] = { { TOK_CARET, BOP_BXOR }, { TOK_EOF, 0 } };
static const BinTok LV_BAND[] = { { TOK_AMP,   BOP_BAND }, { TOK_EOF, 0 } };
static const BinTok LV_EQ[]  = { { TOK_EQ_EQ, BOP_EQ }, { TOK_BANG_EQ, BOP_NE },
                                 { TOK_EOF, 0 } };
static const BinTok LV_CMP[] = { { TOK_LT, BOP_LT }, { TOK_LT_EQ, BOP_LE },
                                 { TOK_GT, BOP_GT }, { TOK_GT_EQ, BOP_GE },
                                 { TOK_EOF, 0 } };
static const BinTok LV_SHIFT[] = { { TOK_SHL, BOP_SHL }, { TOK_SHR, BOP_SHR },
                                   { TOK_EOF, 0 } };
static const BinTok LV_ADD[] = { { TOK_PLUS, BOP_ADD }, { TOK_MINUS, BOP_SUB },
                                 { TOK_EOF, 0 } };
static const BinTok LV_MUL[] = { { TOK_STAR, BOP_MUL }, { TOK_SLASH, BOP_DIV },
                                 { TOK_PERCENT, BOP_MOD }, { TOK_EOF, 0 } };
static const BinTok *const LEVELS[] = { LV_OR, LV_AND, LV_BOR, LV_BXOR, LV_BAND,
                                        LV_EQ, LV_CMP, LV_SHIFT, LV_ADD, LV_MUL };
#define NUM_LEVELS ((int)(sizeof LEVELS / sizeof LEVELS[0]))

static Node *parse_binary_level(Parser *p, int level) {
    if (level >= NUM_LEVELS) return parse_unary(p);

    Node *left = parse_binary_level(p, level + 1);
    for (;;) {
        TokenType t = peek(p).type;
        const BinTok *hit = NULL;
        for (const BinTok *e = LEVELS[level]; e->tok != TOK_EOF; e++)
            if (e->tok == t) { hit = e; break; }
        if (!hit) return left;

        Token op = advance(p);
        Node *right = parse_binary_level(p, level + 1);
        Node *n = node_new(p->arena, NK_BINARY, op.line, op.col);
        n->binary.op = hit->op;
        n->binary.left = left;
        n->binary.right = right;
        left = n;
    }
}

static NodeList *parse_call_args(Parser *p) {
    NodeListBuilder b = {0};
    expect(p, TOK_LPAREN);
    if (!check(p, TOK_RPAREN)) {
        nlb_push(p->arena, &b, parse_expr(p));
        while (match(p, TOK_COMMA)) nlb_push(p->arena, &b, parse_expr(p));
    }
    expect(p, TOK_RPAREN);
    return b.head;
}

/* unary ::= ('-' | '!' | '~' | '&' | '*') unary | ('++' | '--') unary
 *         | 'maatru' '(' expr ',' type ')' | 'alavu' '(' (type | expr) ')'
 *         | 'puthu' IDENT '(' args ')' | postfix                            */
static Node *parse_unary_inner(Parser *p);
static Node *parse_unary(Parser *p) {
    if (++p->nest > MAX_NEST) nest_abort(p);
    Node *r = parse_unary_inner(p);
    p->nest--;
    return r;
}
static Node *parse_unary_inner(Parser *p) {
    Token t = peek(p);
    switch (t.type) {
        case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR: {
            advance(p);
            Node *operand = parse_unary(p);
            Node *n = node_new(p->arena, NK_UNARY, t.line, t.col);
            n->unary.op = t.type == TOK_MINUS ? UOP_NEG  : t.type == TOK_BANG ? UOP_NOT
                        : t.type == TOK_TILDE ? UOP_BNOT : t.type == TOK_AMP  ? UOP_ADDR
                        : UOP_DEREF;
            n->unary.operand = operand;
            return n;
        }
        case TOK_PLUS_PLUS: case TOK_MINUS_MINUS: {
            advance(p);
            Node *operand = parse_unary(p);
            Node *n = node_new(p->arena, NK_INCDEC, t.line, t.col);
            n->incdec.operand = operand;
            n->incdec.is_inc = t.type == TOK_PLUS_PLUS;
            n->incdec.prefix = true;
            return n;
        }
        case TOK_MAATRU: {
            advance(p);
            expect(p, TOK_LPAREN);
            Node *e = parse_expr(p);
            expect(p, TOK_COMMA);
            TypeInfo *to = parse_type(p);
            expect(p, TOK_RPAREN);
            Node *n = node_new(p->arena, NK_CAST, t.line, t.col);
            n->cast.to = to;
            n->cast.expr = e;
            return n;
        }
        case TOK_ALAVU: {
            advance(p);
            expect(p, TOK_LPAREN);
            Node *n = node_new(p->arena, NK_SIZEOF, t.line, t.col);
            Token f = peek(p);
            if (f.type == TOK_SEYAL ||
                (f.type == TOK_IDENT && builtin_type(f.start, f.length)))
                n->size_of.of_type = parse_type(p);
            else
                n->size_of.expr = parse_expr(p);   /* sema also accepts type names */
            expect(p, TOK_RPAREN);
            return n;
        }
        case TOK_PUTHU: {
            advance(p);
            Token name = expect(p, TOK_IDENT);
            TypeInfo *ct = parse_collection_type(p, name);
            if (ct) {
                Node *ln = node_new(p->arena, NK_LIST_NEW, t.line, t.col);
                ln->list_new.ltype = ct;
                ln->list_new.items = parse_call_args(p);
                return ln;
            }
            Node *n = node_new(p->arena, NK_NEW, t.line, t.col);
            n->new_expr.name = lexeme(p, name);
            n->new_expr.args = parse_call_args(p);
            return n;
        }
        default:
            return parse_postfix(p);
    }
}

static Node *parse_postfix(Parser *p) {
    Node *node = parse_primary(p);
    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            Token lb = advance(p);
            Node *idx = parse_expr(p);
            expect(p, TOK_RBRACKET);
            Node *n = node_new(p->arena, NK_INDEX, lb.line, lb.col);
            n->index.array = node;
            n->index.index = idx;
            node = n;
        } else if (check(p, TOK_DOT)) {
            Token dot = advance(p);
            Token fn = expect(p, TOK_IDENT);
            Node *n = node_new(p->arena, NK_MEMBER, dot.line, dot.col);
            n->member.object = node;
            n->member.field  = lexeme(p, fn);
            node = n;
        } else if (check(p, TOK_LPAREN)) {
            Token lp = peek(p);
            NodeList *args = parse_call_args(p);
            if (node->kind == NK_MEMBER) {              /* obj.method(args) */
                Node *n = node_new(p->arena, NK_MCALL, node->line, node->col);
                n->mcall.object = node->member.object;
                n->mcall.method = node->member.field;
                n->mcall.args = args;
                node = n;
            } else {
                if (node->kind != NK_IDENT)
                    syntax_error(p, lp, "only named functions and methods can be called");
                Node *n = node_new(p->arena, NK_CALL, node->line, node->col);
                n->call.name = node->kind == NK_IDENT ? node->ident.name : "?";
                n->call.args = args;
                node = n;
            }
        } else if (check(p, TOK_PLUS_PLUS) || check(p, TOK_MINUS_MINUS)) {
            Token op = advance(p);
            Node *n = node_new(p->arena, NK_INCDEC, op.line, op.col);
            n->incdec.operand = node;
            n->incdec.is_inc = op.type == TOK_PLUS_PLUS;
            n->incdec.prefix = false;
            node = n;
        } else {
            return node;
        }
    }
}

/* $"text {expr} more {expr:.2f}"  ==>  "text " + str(expr) + " more " + str(expr, ".2f") */
static Node *str_lit_node(Parser *p, Token tok, const char *text, size_t n) {
    char *buf = arena_alloc(p->arena, n + 1);
    memcpy(buf, text, n);
    buf[n] = 0;
    Node *l = node_new(p->arena, NK_STRING_LIT, tok.line, tok.col);
    l->string_lit.value = buf;
    return l;
}

static Node *parse_interpolated(Parser *p, Token tok) {
    const char *v = tok.val.str_val;
    Node *acc = NULL;
    size_t lit_start = 0;
    char *lit = arena_alloc(p->arena, strlen(v) + 1);
    size_t ln = 0;
    #define APPEND(part) do { Node *_n = (part); \
        if (!acc) acc = _n; else { Node *b = node_new(p->arena, NK_BINARY, tok.line, tok.col); \
            b->binary.op = BOP_ADD; b->binary.left = acc; b->binary.right = _n; acc = b; } } while (0)
    (void)lit_start;
    for (size_t i = 0; v[i];) {
        if (v[i] == '{' && v[i + 1] == '{') { lit[ln++] = '{'; i += 2; continue; }
        if (v[i] == '}' && v[i + 1] == '}') { lit[ln++] = '}'; i += 2; continue; }
        if (v[i] != '{') { lit[ln++] = v[i++]; continue; }
        size_t j = i + 1;
        int depth = 1;
        bool in_str = false;
        while (v[j] && depth) {
            if (v[j] == '"') in_str = !in_str;
            else if (!in_str && v[j] == '{') depth++;
            else if (!in_str && v[j] == '}') depth--;
            if (depth) j++;
        }
        if (!v[j]) {
            report_error(p->lexer->filename, tok.line, tok.col,
                         "unclosed '{' in the interpolated string (write {{ for a literal brace)");
            return str_lit_node(p, tok, "", 0);
        }
        if (ln) { APPEND(str_lit_node(p, tok, lit, ln)); ln = 0; }
        size_t elen = j - (i + 1);
        char *etext = arena_alloc(p->arena, elen + 1);
        memcpy(etext, v + i + 1, elen);
        etext[elen] = 0;
        /* optional ":format" after the last top-level colon (not part of ?: or a string) */
        const char *spec = NULL;
        if (!strchr(etext, '?')) {
            char *c = strrchr(etext, ':');
            if (c && !strchr(c, '"') && !strchr(c, '(')) { *c = 0; spec = c + 1; }
        }
        Lexer sub;
        lexer_init(&sub, etext, p->lexer->filename, p->arena);
        sub.line = tok.line;
        sub.col = tok.col + 2 + (int)i;
        Parser sp;
        parser_init(&sp, &sub, p->arena);
        sp.func_depth = p->func_depth;
        Node *e = parse_expr(&sp);
        if (lexer_peek(&sub).type != TOK_EOF)
            report_error(p->lexer->filename, tok.line, tok.col + 2 + (int)i,
                         "unexpected text in the interpolation {%s}", etext);
        Node *call = node_new(p->arena, NK_CALL, e->line, e->col);
        call->call.name = "str";
        NodeList *a1 = arena_alloc(p->arena, sizeof *a1);
        a1->node = e; a1->next = NULL;
        call->call.args = a1;
        if (spec) {
            NodeList *a2 = arena_alloc(p->arena, sizeof *a2);
            a2->node = str_lit_node(p, tok, spec, strlen(spec));
            a2->next = NULL;
            a1->next = a2;
        }
        APPEND(call);
        i = j + 1;
    }
    if (ln || !acc) APPEND(str_lit_node(p, tok, lit, ln));
    #undef APPEND
    return acc;
}

static Node *parse_primary(Parser *p) {
    Token tok = peek(p);
    switch (tok.type) {
        case TOK_SEYAL: {                  /* lambda: seyal (a: int): int { ... } */
            static int lam_count = 0;
            char nm[32];
            snprintf(nm, sizeof nm, "_lambda%d", ++lam_count);
            lambda_name_pending = arena_strdup(p->arena, nm);
            Node *fn = parse_func_decl(p, true);
            fn->is_lambda = true;
            Node *lam = node_new(p->arena, NK_LAMBDA, tok.line, tok.col);
            lam->lambda.fn = fn;
            return lam;
        }
        case TOK_INT_LIT: {
            advance(p);
            Node *n = node_new(p->arena, NK_INT_LIT, tok.line, tok.col);
            n->int_lit.value = tok.val.int_val;
            n->int_lit.is_unsigned = tok.uns;
            return n;
        }
        case TOK_CHAR_LIT: {
            advance(p);
            Node *n = node_new(p->arena, NK_INT_LIT, tok.line, tok.col);
            n->int_lit.value = tok.val.int_val;
            n->int_lit.is_char = true;
            return n;
        }
        case TOK_ILLAI: {
            advance(p);
            return node_new(p->arena, NK_NULL_LIT, tok.line, tok.col);
        }
        case TOK_ITHU: {
            advance(p);
            return node_new(p->arena, NK_THIS, tok.line, tok.col);
        }
        case TOK_THANDHAI: {                 /* thandhai(args) | thandhai.m(args) */
            advance(p);
            if (check(p, TOK_LPAREN)) {
                Node *n = node_new(p->arena, NK_CTOR_CALL, tok.line, tok.col);
                n->ctor_call.args = parse_call_args(p);
                return n;
            }
            return node_new(p->arena, NK_SUPER, tok.line, tok.col);
        }
        case TOK_FLOAT_LIT: {
            advance(p);
            Node *n = node_new(p->arena, NK_FLOAT_LIT, tok.line, tok.col);
            n->float_lit.value = tok.val.float_val;
            return n;
        }
        case TOK_ISTRING_LIT:
            advance(p);
            return parse_interpolated(p, tok);
        case TOK_STRING_LIT: {
            advance(p);
            Node *n = node_new(p->arena, NK_STRING_LIT, tok.line, tok.col);
            n->string_lit.value = tok.val.str_val;
            return n;
        }
        case TOK_IDENT: {
            advance(p);
            bool is_true  = tok.length == 4 && !memcmp(tok.start, "true", 4);
            bool is_false = tok.length == 5 && !memcmp(tok.start, "false", 5);
            if (is_true || is_false) {
                Node *n = node_new(p->arena, NK_BOOL_LIT, tok.line, tok.col);
                n->bool_lit.value = is_true;
                return n;
            }
            if (check(p, TOK_LBRACE)) {          /* Name { field: value, ... } */
                advance(p);
                FieldInit *head = NULL, *tail = NULL;
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->panic) {
                    Token fn = expect(p, TOK_IDENT);
                    expect(p, TOK_COLON);
                    FieldInit *fi = arena_alloc(p->arena, sizeof *fi);
                    memset(fi, 0, sizeof *fi);
                    fi->name = lexeme(p, fn);
                    fi->line = fn.line; fi->col = fn.col;
                    fi->value = parse_expr(p);
                    if (tail) tail->next = fi; else head = fi;
                    tail = fi;
                    if (!match(p, TOK_COMMA)) break;
                }
                expect(p, TOK_RBRACE);
                Node *n = node_new(p->arena, NK_STRUCT_LIT, tok.line, tok.col);
                n->struct_lit.name = lexeme(p, tok);
                n->struct_lit.inits = head;
                return n;
            }
            Node *n = node_new(p->arena, NK_IDENT, tok.line, tok.col);
            n->ident.name = lexeme(p, tok);
            return n;
        }
        case TOK_LPAREN: {
            advance(p);
            Node *inner = parse_expr(p);
            expect(p, TOK_RPAREN);
            return inner;
        }
        case TOK_LBRACKET: {
            advance(p);
            NodeListBuilder b = {0};
            if (!check(p, TOK_RBRACKET)) {
                nlb_push(p->arena, &b, parse_expr(p));
                while (match(p, TOK_COMMA))
                    nlb_push(p->arena, &b, parse_expr(p));
            }
            expect(p, TOK_RBRACKET);
            Node *n = node_new(p->arena, NK_ARRAY_INIT, tok.line, tok.col);
            n->array_init.elements = b.head;
            return n;
        }
        default: {
            /* Lexer already reported TOK_ERROR; don't double-report. */
            if (tok.type == TOK_ERROR) {
                advance(p);
                p->panic = true;
            } else {
                syntax_error(p, tok, "expected an expression but found %s",
                             token_type_name(tok.type));
            }
            /* Deliberately do not consume: the caller's loop guarantees
             * progress, and sync points like ';' and '}' must stay intact. */
            Node *dummy = node_new(p->arena, NK_INT_LIT, tok.line, tok.col);
            return dummy;
        }
    }
}
