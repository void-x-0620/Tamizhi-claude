/*
 * sema.c — Semantic analysis (see sema.h).
 */
#include "sema.h"
#include <limits.h>

/* ─── Scopes ───────────────────────────────────────────────────────────── */
static unsigned sym_hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h % SCOPE_BUCKETS;
}

static void scope_push(Sema *s) {
    Scope *sc = arena_alloc(s->arena, sizeof(Scope));
    memset(sc->buckets, 0, sizeof sc->buckets);
    sc->parent = s->scope;
    s->scope = sc;
}

static void scope_pop(Sema *s) {
    Symbol *unused[64]; int nu = 0;
    for (int b = 0; b < SCOPE_BUCKETS; b++)
        for (Symbol *y = s->scope->buckets[b]; y; y = y->next)
            if (y->track && !y->used && y->name[0] != '_' && nu < 64) {
                int i = nu++;                       /* keep them in source order */
                while (i > 0 && (unused[i-1]->line > y->line ||
                       (unused[i-1]->line == y->line && unused[i-1]->col > y->col))) {
                    unused[i] = unused[i-1]; i--;
                }
                unused[i] = y;
            }
    for (int i = 0; i < nu; i++) {
        diag_hint("remove it, or start the name with '_' to silence this");
        report_warning(s->filename, unused[i]->line, unused[i]->col,
                       "variable '%s' is never used", unused[i]->name);
    }
    s->scope = s->scope->parent;
}

static Symbol *lookup_in(Scope *sc, const char *name) {
    for (Symbol *y = sc->buckets[sym_hash(name)]; y; y = y->next)
        if (!strcmp(y->name, name)) return y;
    return NULL;
}

/* A lambda being checked: the locals it uses from outside become captures. */
typedef struct LambdaCtx {
    Scope *boundary;               /* the lambda's own parameter scope */
    Capture *caps, *tail;
    int ncap;
    struct LambdaCtx *outer;
} LambdaCtx;

static void add_capture(Sema *s, LambdaCtx *l, Symbol *y) {
    for (Capture *c = l->caps; c; c = c->next)
        if (c->sym == y) return;
    Capture *c = arena_alloc(s->arena, sizeof *c);
    c->name = y->name; c->type = y->type; c->sym = y; c->next = NULL;
    if (l->tail) l->tail->next = c; else l->caps = c;
    l->tail = c;
    l->ncap++;
}

static Symbol *lookup(Sema *s, const char *name);
static void func_decl(Sema *s, Node *n);

/* Like lookup(), but a local of an enclosing function used inside a lambda
 * is recorded as a capture of every lambda between the use and the owner. */
static Symbol *lookup_cap(Sema *s, const char *name, void **capslot) {
    Scope *found = NULL;
    Symbol *y = NULL;
    for (Scope *sc = s->scope; sc; sc = sc->parent) {
        y = lookup_in(sc, name);
        if (y) { found = sc; break; }
    }
    if (capslot) *capslot = NULL;
    if (!y || y->kind != SYM_VAR || found == s->global || !s->lam) return y;
    for (LambdaCtx *l = s->lam; l; l = l->outer) {
        bool outside = false;
        for (Scope *sc = l->boundary ? l->boundary->parent : NULL; sc; sc = sc->parent)
            if (sc == found) { outside = true; break; }
        if (!outside) break;
        add_capture(s, l, y);
        if (l == s->lam && capslot) *capslot = y;
    }
    return y;
}

static Symbol *lookup(Sema *s, const char *name) {
    for (Scope *sc = s->scope; sc; sc = sc->parent) {
        Symbol *y = lookup_in(sc, name);
        if (y) return y;
    }
    return NULL;
}

/* Types live only at the top level, so a local variable never hides one. */
static Symbol *lookup_type(Sema *s, const char *name) {
    Symbol *y = lookup_in(s->global, name);
    if (y && (y->kind == SYM_STRUCT || y->kind == SYM_ENUM ||
              y->kind == SYM_CLASS || y->kind == SYM_TYPEDEF))
        return y;
    return NULL;
}

static Symbol *declare(Sema *s, const char *name, SymKind kind, TypeInfo *type,
                       int line, int col) {
    if (lookup_in(s->scope, name)) {
        report_error(s->filename, line, col,
                     "duplicate declaration of '%s'", name);
        return NULL;
    }
    Symbol *y = arena_alloc(s->arena, sizeof(Symbol));
    memset(y, 0, sizeof *y);
    y->name = name;
    y->kind = kind;
    y->type = type;
    unsigned h = sym_hash(name);
    y->next = s->scope->buckets[h];
    s->scope->buckets[h] = y;
    return y;
}

/* "did you mean" for names: scan every visible scope. */
static void suggest_symbol(Sema *s, const char *name, bool funcs_only) {
    const char *c[512]; int n = 0;
    for (Scope *sc = s->scope; sc; sc = sc->parent)
        for (int b = 0; b < SCOPE_BUCKETS; b++)
            for (Symbol *y = sc->buckets[b]; y && n < 512; y = y->next)
                if (!funcs_only || y->kind == SYM_FUNC ||
                    (y->kind == SYM_VAR && y->type && y->type->kind == TY_FUNC))
                    c[n++] = y->name;
    const char *m = best_match(name, c, n);
    if (m) diag_hint("did you mean '%s'?", m);
}

static void suggest_type(Sema *s, const char *name) {
    const char *c[512]; int n = 0;
    static const char *prim[] = { "int", "float", "string", "bool", "void", "char",
        "uint", "long", "short", "uchar", "ushort", "ulong", "usize", "isize",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "List" };
    for (size_t i = 0; i < sizeof prim / sizeof *prim; i++) c[n++] = prim[i];
    for (int b = 0; b < SCOPE_BUCKETS; b++)
        for (Symbol *y = s->global->buckets[b]; y && n < 512; y = y->next)
            if (y->kind == SYM_STRUCT || y->kind == SYM_ENUM ||
                y->kind == SYM_CLASS || y->kind == SYM_TYPEDEF) c[n++] = y->name;
    const char *m = best_match(name, c, n);
    if (m) diag_hint("did you mean '%s'?", m);
}

/* printf into the arena. */
static const char *tfmt(Sema *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static const char *tfmt(Sema *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = arena_alloc(s->arena, (size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

/* ─── Type predicates ──────────────────────────────────────────────────── */
static bool is_unknown(TypeInfo *t) { return !t || t->kind == TY_UNKNOWN; }
static bool is_arith(TypeInfo *t) {
    return is_unknown(t) || t->kind == TY_INT || t->kind == TY_FLOAT ||
           t->kind == TY_BOOL || t->kind == TY_ENUM;
}
static bool is_integral(TypeInfo *t) {
    return is_unknown(t) || t->kind == TY_INT || t->kind == TY_BOOL ||
           t->kind == TY_ENUM;
}
static bool is_scalar(TypeInfo *t) {
    return is_arith(t) || t->kind == TY_POINTER || t->kind == TY_CLASS ||
           t->kind == TY_STRING || t->kind == TY_FUNC || t->kind == TY_NULL ||
           t->kind == TY_LIST || t->kind == TY_MAP;
}
static bool is_ptrish(TypeInfo *t) {               /* can be compared to illai */
    return t->kind == TY_POINTER || t->kind == TY_CLASS ||
           t->kind == TY_STRING || t->kind == TY_FUNC || t->kind == TY_NULL ||
           t->kind == TY_LIST || t->kind == TY_MAP;
}
static bool is_byte(TypeInfo *t) {                 /* char-like element */
    return t && t->kind == TY_INT && t->bits == 8;
}

static const char *type_str(Sema *s, TypeInfo *t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TY_VOID:    return "void";
        case TY_INT: case TY_FLOAT: return t->tname;
        case TY_STRING:  return "string";
        case TY_BOOL:    return "bool";
        case TY_NULL:    return "illai";
        case TY_STRUCT: case TY_ENUM: case TY_CLASS: return t->struct_name;
        case TY_UNKNOWN: return "unknown";
        case TY_POINTER: return tfmt(s, "%s*", type_str(s, t->elem_type));
        case TY_LIST:    return tfmt(s, "List<%s>", type_str(s, t->elem_type));
        case TY_MAP:     return t->is_set ? tfmt(s, "Set<%s>", type_str(s, t->key_type))
                                          : tfmt(s, "Map<%s, %s>", type_str(s, t->key_type),
                                                 type_str(s, t->elem_type));
        case TY_ARRAY:
            if (t->array_size > 0)
                return tfmt(s, "%s[%d]", type_str(s, t->elem_type), t->array_size);
            return tfmt(s, "%s[]", type_str(s, t->elem_type));
        case TY_CLOSURE: {
            const char *r = "Fn<(";
            for (Param *p = t->fparams; p; p = p->next)
                r = tfmt(s, "%s%s%s", r, type_str(s, p->type), p->next ? ", " : "");
            return tfmt(s, "%s): %s>", r, type_str(s, t->fret));
        }
        case TY_FUNC: {
            const char *r = "seyal(";
            for (Param *p = t->fparams; p; p = p->next)
                r = tfmt(s, "%s%s%s", r, type_str(s, p->type), p->next ? ", " : "");
            return tfmt(s, "%s):%s", r, type_str(s, t->fret));
        }
    }
    return "unknown";
}

/* ─── Constant evaluation (integers only) ──────────────────────────────── */
static bool const_eval(Node *n, long long *out) {
    switch (n->kind) {
        case NK_INT_LIT:  *out = n->int_lit.value; return true;
        case NK_BOOL_LIT: *out = n->bool_lit.value; return true;
        case NK_UNARY: {
            long long v;
            if (n->unary.op == UOP_ADDR || n->unary.op == UOP_DEREF) return false;
            if (!const_eval(n->unary.operand, &v)) return false;
            *out = n->unary.op == UOP_NEG ? (long long)(0ULL - (unsigned long long)v)
                 : n->unary.op == UOP_BNOT ? ~v : !v;
            return true;
        }
        case NK_BINARY: {
            long long a, b;
            if (!const_eval(n->binary.left, &a) || !const_eval(n->binary.right, &b))
                return false;
            unsigned long long ua = (unsigned long long)a, ub = (unsigned long long)b;
            switch (n->binary.op) {
                case BOP_ADD: *out = (long long)(ua + ub); return true;
                case BOP_SUB: *out = (long long)(ua - ub); return true;
                case BOP_MUL: *out = (long long)(ua * ub); return true;
                case BOP_DIV: if (!b || (a == LLONG_MIN && b == -1)) return false;
                              *out = a / b; return true;
                case BOP_MOD: if (!b || (a == LLONG_MIN && b == -1)) return false;
                              *out = a % b; return true;
                case BOP_EQ:  *out = a == b; return true;
                case BOP_NE:  *out = a != b; return true;
                case BOP_LT:  *out = a <  b; return true;
                case BOP_LE:  *out = a <= b; return true;
                case BOP_GT:  *out = a >  b; return true;
                case BOP_GE:  *out = a >= b; return true;
                case BOP_AND: *out = a && b; return true;
                case BOP_OR:  *out = a || b; return true;
                case BOP_BAND: *out = a & b; return true;
                case BOP_BOR:  *out = a | b; return true;
                case BOP_BXOR: *out = a ^ b; return true;
                case BOP_SHL: if (b < 0 || b > 62 || a < 0) return false;
                              *out = (long long)(ua << b); return true;
                case BOP_SHR: if (b < 0 || b > 63 || a < 0) return false;
                              *out = a >> b; return true;
            }
            return false;
        }
        case NK_TERNARY: {
            long long c;
            if (!const_eval(n->ternary.cond, &c)) return false;
            return const_eval(c ? n->ternary.then_e : n->ternary.else_e, out);
        }
        case NK_CAST:
            return n->cast.to && n->cast.to->kind == TY_INT &&
                   const_eval(n->cast.expr, out);
        default: return false;
    }
}

/* Can the integer value v be stored in the integer type d? */
static bool value_fits(TypeInfo *d, long long v, bool src_unsigned) {
    if (d->kind == TY_BOOL) return true;
    if (d->kind != TY_INT) return true;
    if (d->bits >= 64) return d->is_unsigned ? (v >= 0 || src_unsigned)
                                             : (v >= 0 || !src_unsigned);
    if (src_unsigned && v < 0) return false;
    if (d->is_unsigned) return v >= 0 && v < (1LL << d->bits);
    return v >= -(1LL << (d->bits - 1)) && v < (1LL << (d->bits - 1));
}

/* ─── Classes ──────────────────────────────────────────────────────────── */
static Node *class_of_type(Sema *s, TypeInfo *t) {
    if (!t || t->kind != TY_CLASS) return NULL;
    Symbol *y = lookup_type(s, t->struct_name);
    return y && y->kind == SYM_CLASS ? y->decl : NULL;
}

static bool is_subclass(Node *d, Node *b) {
    for (; d; d = d->class_decl.parent_decl)
        if (d == b) return true;
    return false;
}

static Method *find_method(Node *cls, const char *name, Node **owner) {
    for (Node *c = cls; c; c = c->class_decl.parent_decl)
        for (Method *m = c->class_decl.methods; m; m = m->next)
            if (!m->is_ctor && !strcmp(m->name, name)) {
                if (owner) *owner = c;
                return m;
            }
    return NULL;
}

static Field *find_cfield(Node *cls, const char *name, Node **owner, int *hops) {
    int h = 0;
    for (Node *c = cls; c; c = c->class_decl.parent_decl, h++)
        for (Field *f = c->class_decl.fields; f; f = f->next)
            if (!strcmp(f->name, name)) {
                if (owner) *owner = c;
                if (hops) *hops = h;
                return f;
            }
    return NULL;
}

static void suggest_fields(Field *f, const char *name) {
    const char *c[256]; int n = 0;
    for (; f && n < 256; f = f->next) c[n++] = f->name;
    const char *m = best_match(name, c, n);
    if (m) diag_hint("did you mean '%s'?", m);
}

static void suggest_members(Node *cls, const char *name, bool methods) {
    const char *c[256]; int n = 0;
    for (Node *k = cls; k; k = k->class_decl.parent_decl) {
        if (methods) { for (Method *m = k->class_decl.methods; m && n < 256; m = m->next) if (!m->is_ctor) c[n++] = m->name; }
        else for (Field *f = k->class_decl.fields; f && n < 256; f = f->next) c[n++] = f->name;
    }
    const char *m = best_match(name, c, n);
    if (m) diag_hint("did you mean '%s'?", m);
}

static Method *eff_ctor(Node *cls) {               /* nearest constructor */
    for (Node *c = cls; c; c = c->class_decl.parent_decl)
        if (c->class_decl.ctor) return c->class_decl.ctor;
    return NULL;
}

static bool access_ok(Sema *s, Access a, Node *owner) {
    if (a == ACC_PUBLIC) return true;
    if (!s->cur_class) return false;
    if (a == ACC_PRIVATE) return s->cur_class == owner;
    return is_subclass(s->cur_class, owner);
}

static void check_access(Sema *s, Access a, Node *owner, const char *what,
                         const char *name, int line, int col) {
    if (access_ok(s, a, owner)) return;
    report_error(s->filename, line, col, "%s '%s' is %s in class '%s'", what,
                 name, a == ACC_PRIVATE ? "private" : "protected",
                 owner->class_decl.name);
}

/* ─── Type resolution ──────────────────────────────────────────────────── */
static void enum_resolve(Sema *s, Symbol *y);
static TypeInfo *expr(Sema *s, Node *n);
static void      stmt(Sema *s, Node *n);

/* Turn the parser's by-name placeholders into struct / enum / class types
 * (typedefs are expanded in place). Reports unknown names. */
static void resolve_type(Sema *s, TypeInfo *t, int line, int col) {
    if (!t) return;
    switch (t->kind) {
        case TY_ARRAY: case TY_POINTER: case TY_LIST:
            resolve_type(s, t->elem_type, line, col);
            return;
        case TY_MAP:
            resolve_type(s, t->key_type, line, col);
            resolve_type(s, t->elem_type, line, col);
            if (!is_unknown(t->elem_type) && !t->is_set &&
                (t->elem_type->kind == TY_FUNC || t->elem_type->kind == TY_ARRAY ||
                 t->elem_type->kind == TY_VOID))
                report_error(s->filename, line, col,
                             "Map values cannot be %s; wrap it in a struct or class",
                             t->elem_type->kind == TY_FUNC ? "function types" :
                             t->elem_type->kind == TY_ARRAY ? "arrays" : "void");
            if (!is_unknown(t->key_type) && t->key_type->kind != TY_INT &&
                t->key_type->kind != TY_BOOL && t->key_type->kind != TY_ENUM &&
                t->key_type->kind != TY_STRING)
                report_error(s->filename, line, col,
                             "%s keys must be an integer, char, bool, enum or string, not %s",
                             t->is_set ? "Set" : "Map", type_str(s, t->key_type));
            return;
        case TY_FUNC: case TY_CLOSURE:
            for (Param *p = t->fparams; p; p = p->next)
                resolve_type(s, p->type, line, col);
            resolve_type(s, t->fret, line, col);
            return;
        case TY_STRUCT: break;
        default: return;
    }
    Symbol *y = lookup_type(s, t->struct_name);
    if (!y) {
        suggest_type(s, t->struct_name);
        report_error(s->filename, line, col, "unknown type '%s'", t->struct_name);
        t->kind = TY_UNKNOWN;
        return;
    }
    switch (y->kind) {
        case SYM_ENUM:  t->kind = TY_ENUM;  break;
        case SYM_CLASS: t->kind = TY_CLASS; break;
        case SYM_TYPEDEF:
            if (y->state == 1) {
                report_error(s->filename, line, col,
                             "typedef '%s' refers to itself", y->name);
                t->kind = TY_UNKNOWN;
                return;
            }
            if (y->state == 0) {
                y->state = 1;
                resolve_type(s, y->type, line, col);
                y->state = 2;
            }
            *t = *y->type;
            break;
        default: break;
    }
}

static Field *find_field(Field *f, const char *name) {
    for (; f; f = f->next)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}

/* ─── Conversions ──────────────────────────────────────────────────────── */
static TypeInfo *promote(TypeInfo *t) {
    if ((t->kind == TY_INT && t->bits >= 32) || t->kind == TY_FLOAT) return t;
    return &ty_i32;                            /* bool, enum, char, short ... */
}

/* C's usual arithmetic conversions, on Tamizhi's type descriptors. */
static TypeInfo *arith_result(TypeInfo *a, TypeInfo *b) {
    if (is_unknown(a) || is_unknown(b)) return &ty_unknown;
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) {
        if (a->kind == TY_FLOAT && b->kind == TY_FLOAT) return b->bits > a->bits ? b : a;
        return a->kind == TY_FLOAT ? a : b;
    }
    a = promote(a); b = promote(b);
    if (a->bits != b->bits) return a->bits > b->bits ? a : b;
    if (a->is_unsigned) return a;
    if (b->is_unsigned) return b;
    return a;
}

static bool int_fits(TypeInfo *d, TypeInfo *src, Node *node) {
    long long v;
    if (node && const_eval(node, &v)) {
        bool u = node->kind == NK_INT_LIT && node->int_lit.is_unsigned;
        return value_fits(d, v, u);
    }
    int  sb = src->kind == TY_INT ? src->bits : 32;
    bool su = src->kind == TY_INT && src->is_unsigned;
    if (su == d->is_unsigned) return d->bits >= sb;
    if (su && !d->is_unsigned) return d->bits > sb;
    return false;
}

/* May a value of type src (produced by node) be stored into d without an
 * explicit maatru? */
static bool assignable(Sema *s, TypeInfo *d, TypeInfo *src, Node *node) {
    if (is_unknown(d) || is_unknown(src)) return true;
    if (d->kind == TY_VOID || src->kind == TY_VOID) return false;
    switch (d->kind) {
        case TY_BOOL:  return is_arith(src);
        case TY_INT:
            if (src->kind == TY_BOOL || src->kind == TY_ENUM) return true;
            return src->kind == TY_INT && int_fits(d, src, node);
        case TY_FLOAT:
            if (src->kind == TY_FLOAT)
                return src->bits <= d->bits || (node && node->kind == NK_FLOAT_LIT);
            return src->kind == TY_INT || src->kind == TY_BOOL || src->kind == TY_ENUM;
        case TY_ENUM:
            return src->kind == TY_ENUM && !strcmp(src->struct_name, d->struct_name);
        case TY_STRING:
            return src->kind == TY_STRING || src->kind == TY_NULL ||
                   (src->kind == TY_POINTER && is_byte(src->elem_type));
        case TY_POINTER:
            if (src->kind == TY_NULL) return true;
            if (src->kind == TY_STRING)
                return is_byte(d->elem_type) || d->elem_type->kind == TY_VOID;
            if (src->kind == TY_POINTER || src->kind == TY_ARRAY) {
                TypeInfo *de = d->elem_type, *se = src->elem_type;
                if (is_unknown(de) || is_unknown(se)) return true;
                if (de->kind == TY_VOID) return true;
                if (src->kind == TY_POINTER && se->kind == TY_VOID) return true;
                if (is_byte(de) && is_byte(se)) return true;
                return type_same(de, se);
            }
            return false;
        case TY_ARRAY:
            if (src->kind != TY_ARRAY) return false;
            if (is_unknown(d->elem_type) || is_unknown(src->elem_type)) return true;
            return type_same(d->elem_type, src->elem_type);
        case TY_STRUCT:
            return src->kind == TY_STRUCT && !strcmp(src->struct_name, d->struct_name);
        case TY_CLASS: {
            if (src->kind == TY_NULL) return true;
            if (src->kind != TY_CLASS) return false;
            return is_subclass(class_of_type(s, src), class_of_type(s, d));
        }
        case TY_FUNC:
            return src->kind == TY_NULL ||
                   (src->kind == TY_FUNC && type_same(d, src));
        case TY_CLOSURE: {       /* a closure, or any plain function of the same shape */
            if (src->kind != TY_CLOSURE && src->kind != TY_FUNC) return false;
            TypeInfo tmp = *src;
            tmp.kind = TY_CLOSURE;
            return type_same(d, &tmp);
        }
        case TY_MAP:
            return src->kind == TY_NULL ||
                   (src->kind == TY_MAP && d->is_set == src->is_set &&
                    (is_unknown(d->key_type) || is_unknown(src->key_type) ||
                     type_same(d->key_type, src->key_type)) &&
                    (is_unknown(d->elem_type) || is_unknown(src->elem_type) ||
                     type_same(d->elem_type, src->elem_type)));
        case TY_LIST:
            return src->kind == TY_NULL ||
                   (src->kind == TY_LIST &&
                    (is_unknown(d->elem_type) || is_unknown(src->elem_type) ||
                     type_same(d->elem_type, src->elem_type)));
        default: return false;
    }
}

static const char *conv_hint(Sema *s, TypeInfo *d, TypeInfo *src) {
    if (!is_unknown(d) && !is_unknown(src) && is_arith(d) && is_arith(src) &&
        d->kind != TY_BOOL)
        return tfmt(s, "; use maatru(value, %s) to convert explicitly",
                    type_str(s, d));
    return "";
}

/* The root object an lvalue designates is nilai (const)? */
static const char *const_root(Sema *s, Node *n) {
    switch (n->kind) {
        case NK_IDENT: {
            Symbol *y = lookup(s, n->ident.name);
            return y && y->kind == SYM_VAR && y->is_const ? n->ident.name : NULL;
        }
        case NK_MEMBER:
            if (n->member.object->type && n->member.object->type->kind == TY_STRUCT)
                return const_root(s, n->member.object);
            return NULL;
        case NK_INDEX:
            if (n->index.array->type && n->index.array->type->kind == TY_ARRAY)
                return const_root(s, n->index.array);
            if (n->index.array->type && n->index.array->type->kind == TY_STRING)
                return "a string";
            return NULL;
        case NK_UNARY:
            if (n->unary.op == UOP_DEREF && n->unary.operand->type &&
                n->unary.operand->type->kind == TY_STRING)
                return "a string";
            return NULL;
        default: return NULL;
    }
}

static void check_modifiable(Sema *s, Node *target) {
    if (target->kind == NK_INDEX) target->as_lvalue = true;
    const char *c = const_root(s, target);
    if (c)
        report_error(s->filename, target->line, target->col,
                     "cannot modify %s%s%s: it is nilai (read-only)",
                     c[0] == 'a' && c[1] == ' ' ? "" : "'", c,
                     c[0] == 'a' && c[1] == ' ' ? "" : "'");
}

static bool is_lvalue(Node *n) {
    return n->kind == NK_IDENT || n->kind == NK_INDEX || n->kind == NK_MEMBER ||
           (n->kind == NK_UNARY && n->unary.op == UOP_DEREF);
}

/* Is n usable as a C constant initialiser (static storage)? */
static bool is_const_expr(Node *n) {
    switch (n->kind) {
        case NK_INT_LIT: case NK_FLOAT_LIT: case NK_BOOL_LIT: case NK_STRING_LIT:
        case NK_NULL_LIT: case NK_SIZEOF:
            return true;
        case NK_UNARY:
            return n->unary.op != UOP_ADDR && n->unary.op != UOP_DEREF &&
                   is_const_expr(n->unary.operand);
        case NK_BINARY:
            if (n->binary.left->type && n->binary.left->type->kind == TY_STRING)
                return false;
            return is_const_expr(n->binary.left) && is_const_expr(n->binary.right);
        case NK_TERNARY:
            return is_const_expr(n->ternary.cond) && is_const_expr(n->ternary.then_e) &&
                   is_const_expr(n->ternary.else_e);
        case NK_CAST:
            return is_const_expr(n->cast.expr);
        case NK_ARRAY_INIT:
            for (NodeList *e = n->array_init.elements; e; e = e->next)
                if (!is_const_expr(e->node)) return false;
            return true;
        default: return false;
    }
}

/* ─── Expressions ──────────────────────────────────────────────────────── */
static void cond_check(Sema *s, Node *cond, const char *what) {
    TypeInfo *t = expr(s, cond);
    if (cond->kind == NK_ASSIGN && cond->assign.op == BOP_NONE) {
        diag_hint("write '==' to compare; to assign on purpose, do it on a separate line");
        report_warning(s->filename, cond->line, cond->col,
                       "assignment used as %s", what);
    }
    if (!is_scalar(t))
        report_error(s->filename, cond->line, cond->col,
                     "%s must be a number, bool or pointer, not %s",
                     what, type_str(s, t));
}

static void check_pathi(Sema *s, Node *n) {
    Node *fmt = n->pathi.fmt;
    expr(s, fmt);
    if (fmt->kind != NK_STRING_LIT) {
        report_error(s->filename, fmt->line, fmt->col,
                     "the first argument of 'pathi' must be a string literal");
        for (NodeList *a = n->pathi.args; a; a = a->next) expr(s, a->node);
        return;
    }

    char kinds[64];
    int  count = 0;
    const char *err;
    char *rewritten = fmt_rewrite(s->arena, fmt->string_lit.value, kinds,
                                  (int)sizeof kinds, &count, &err);
    (void)rewritten;
    if (err) {
        report_error(s->filename, fmt->line, fmt->col, "bad format string: %s", err);
        for (NodeList *a = n->pathi.args; a; a = a->next) expr(s, a->node);
        return;
    }

    int i = 0;
    for (NodeList *a = n->pathi.args; a; a = a->next, i++) {
        TypeInfo *t = expr(s, a->node);
        if (i >= count) continue;
        bool ok = true;
        switch (kinds[i]) {
            case 'i': case 'c': ok = is_integral(t);                 break;
            case 'f': ok = is_unknown(t) || t->kind == TY_FLOAT;     break;
            case 's': ok = is_unknown(t) || t->kind == TY_STRING ||
                           (t->kind == TY_POINTER && is_byte(t->elem_type)); break;
        }
        if (!ok)
            report_error(s->filename, a->node->line, a->node->col,
                         "format conversion #%d expects %s but the argument "
                         "is %s", i + 1,
                         kinds[i] == 'f' ? "a float" :
                         kinds[i] == 's' ? "a string" : "an integer",
                         type_str(s, t));
    }
    if (i != count)
        report_error(s->filename, n->line, n->col,
                     "format string has %d conversion(s) but %d argument(s) "
                     "were given", count, i);
}

/* Check call arguments against a parameter list. */
static void check_args(Sema *s, Param *params, NodeList *args, const char *name,
                       int line, int col) {
    Param *par = params;
    NodeList *arg = args;
    int idx = 1;
    for (; par && arg; par = par->next, arg = arg->next, idx++) {
        TypeInfo *at = expr(s, arg->node);
        if (par->type && !assignable(s, par->type, at, arg->node))
            report_error(s->filename, arg->node->line, arg->node->col,
                         "argument %d of '%s': expected %s but got %s%s",
                         idx, name, type_str(s, par->type), type_str(s, at),
                         conv_hint(s, par->type, at));
    }
    bool extra = arg != NULL;
    for (; arg; arg = arg->next) expr(s, arg->node);
    if (par || extra) {
        int want = 0, got = 0;
        for (Param *q = params; q; q = q->next) want++;
        for (NodeList *a = args; a; a = a->next) got++;
        report_error(s->filename, line, col,
                     "'%s' expects %d argument%s but %d %s given", name, want,
                     want == 1 ? "" : "s", got, got == 1 ? "was" : "were");
    }
}

static void expr_list(Sema *s, NodeList *l) {
    for (; l; l = l->next) expr(s, l->node);
}

static TypeInfo *func_value_type(Sema *s, Symbol *y) {
    return type_func(s->arena, y->params, y->type);
}

static TypeInfo *member_expr(Sema *s, Node *n);
static TypeInfo *mcall_expr(Sema *s, Node *n);

static TypeInfo *binary_expr(Sema *s, Node *n) {
    Node *L = n->binary.left, *R = n->binary.right;
    TypeInfo *lt = expr(s, L), *rt = expr(s, R);
    BinOp op = n->binary.op;
    bool cmp = op == BOP_EQ || op == BOP_NE || op == BOP_LT ||
               op == BOP_LE || op == BOP_GT || op == BOP_GE;
    bool logic = op == BOP_AND || op == BOP_OR;
    bool unk = is_unknown(lt) || is_unknown(rt);
    TypeInfo *fail = (cmp || logic) ? &ty_bool : &ty_unknown;

    if (logic) {
        if (!is_scalar(lt) || !is_scalar(rt)) {
            report_error(s->filename, n->line, n->col,
                         "invalid operands to '%s': %s and %s", binop_name(op),
                         type_str(s, lt), type_str(s, rt));
        }
        return &ty_bool;
    }
    if (!unk && lt->kind == TY_STRING && rt->kind == TY_STRING) {
        if (cmp) return &ty_bool;                      /* lowered to strcmp */
        if (op == BOP_ADD) return &ty_string;          /* heap-allocated concat */
    }
    if (!unk && cmp && is_ptrish(lt) && is_ptrish(rt)) {
        bool eq = op == BOP_EQ || op == BOP_NE;
        bool ok = lt->kind == TY_NULL || rt->kind == TY_NULL ||
                  assignable(s, lt, rt, R) || assignable(s, rt, lt, L);
        if (!ok || (!eq && (lt->kind == TY_NULL || rt->kind == TY_NULL ||
                            lt->kind == TY_CLASS || lt->kind == TY_FUNC)))
            report_error(s->filename, n->line, n->col,
                         "invalid operands to '%s': %s and %s", binop_name(op),
                         type_str(s, lt), type_str(s, rt));
        return &ty_bool;
    }
    /* pointer arithmetic */
    if (!unk && (op == BOP_ADD || op == BOP_SUB)) {
        if (lt->kind == TY_POINTER && is_integral(rt)) return lt;
        if (op == BOP_ADD && rt->kind == TY_POINTER && is_integral(lt)) return rt;
        if (op == BOP_SUB && lt->kind == TY_POINTER && rt->kind == TY_POINTER) {
            if (!type_same(lt->elem_type, rt->elem_type))
                report_error(s->filename, n->line, n->col,
                             "cannot subtract %s from %s", type_str(s, rt),
                             type_str(s, lt));
            return &ty_isize;
        }
    }
    if (!is_arith(lt) || !is_arith(rt)) {
        report_error(s->filename, n->line, n->col,
                     "invalid operands to '%s': %s and %s",
                     binop_name(op), type_str(s, lt), type_str(s, rt));
        return fail;
    }
    bool intonly = op == BOP_MOD || op == BOP_BAND || op == BOP_BOR ||
                   op == BOP_BXOR || op == BOP_SHL || op == BOP_SHR;
    if (intonly && (!is_integral(lt) || !is_integral(rt))) {
        report_error(s->filename, n->line, n->col,
                     "'%s' requires integer operands, not %s and %s",
                     binop_name(op), type_str(s, lt), type_str(s, rt));
        return &ty_unknown;
    }
    long long cv;
    if ((op == BOP_DIV || op == BOP_MOD) && const_eval(R, &cv) && cv == 0)
        report_error(s->filename, R->line, R->col, "division by zero");
    if ((op == BOP_SHL || op == BOP_SHR) && !unk && const_eval(R, &cv)) {
        TypeInfo *pl = promote(lt);
        if (cv < 0 || cv >= pl->bits)
            report_error(s->filename, R->line, R->col,
                         "shift count %lld is out of range for %s", cv,
                         type_str(s, pl));
    }
    if (cmp) return &ty_bool;
    if (op == BOP_SHL || op == BOP_SHR) return unk ? &ty_unknown : promote(lt);
    return arith_result(lt, rt);
}

static TypeInfo *unary_expr(Sema *s, Node *n) {
    Node *o = n->unary.operand;
    UnaryOp op = n->unary.op;

    if (op == UOP_ADDR) {
        TypeInfo *t = expr(s, o);
        if (is_unknown(t)) return &ty_unknown;
        if (!is_lvalue(o)) {
            report_error(s->filename, n->line, n->col,
                         "cannot take the address of this expression");
            return &ty_unknown;
        }
        if (t->kind == TY_ARRAY) {
            report_error(s->filename, n->line, n->col,
                         "cannot take the address of an array; use &a[0]");
            return &ty_unknown;
        }
        if (const_root(s, o)) {
            report_error(s->filename, n->line, n->col,
                         "cannot take the address of a nilai variable "
                         "(pointers to const are not supported)");
            return &ty_unknown;
        }
        return type_pointer(s->arena, t);
    }
    TypeInfo *t = expr(s, o);
    if (op == UOP_DEREF) {
        if (is_unknown(t)) return &ty_unknown;
        if (t->kind == TY_STRING) return &ty_char;
        if (t->kind != TY_POINTER) {
            report_error(s->filename, n->line, n->col,
                         "cannot dereference a value of type %s", type_str(s, t));
            return &ty_unknown;
        }
        if (t->elem_type->kind == TY_VOID) {
            report_error(s->filename, n->line, n->col,
                         "cannot dereference a void*; cast it with maatru first");
            return &ty_unknown;
        }
        return t->elem_type;
    }
    if (op == UOP_NOT) {
        if (!is_scalar(t))
            report_error(s->filename, n->line, n->col,
                         "invalid operand to '!': %s", type_str(s, t));
        return &ty_bool;
    }
    if (!is_arith(t) || (op == UOP_BNOT && !is_integral(t))) {
        report_error(s->filename, n->line, n->col,
                     "invalid operand to '%s': %s", unaryop_name(op),
                     type_str(s, t));
        return &ty_unknown;
    }
    return is_unknown(t) ? t : promote(t);
}

static TypeInfo *cast_expr(Sema *s, Node *n) {
    TypeInfo *to = n->cast.to;
    resolve_type(s, to, n->line, n->col);
    TypeInfo *from = expr(s, n->cast.expr);
    if (is_unknown(to) || is_unknown(from)) return to ? to : &ty_unknown;
    bool ok = false;
    switch (to->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL:
            ok = is_arith(from) || (to->kind == TY_INT && from->kind == TY_POINTER);
            break;
        case TY_ENUM: ok = is_arith(from); break;
        case TY_POINTER:
            ok = from->kind == TY_POINTER || from->kind == TY_STRING ||
                 from->kind == TY_NULL || from->kind == TY_ARRAY ||
                 from->kind == TY_INT || from->kind == TY_CLASS;
            break;
        case TY_STRING:
            ok = from->kind == TY_POINTER || from->kind == TY_STRING ||
                 from->kind == TY_NULL;
            break;
        case TY_CLASS: {
            Node *a = class_of_type(s, to), *b = class_of_type(s, from);
            ok = from->kind == TY_NULL ||
                 (b && (is_subclass(a, b) || is_subclass(b, a)));
            break;
        }
        case TY_FUNC: ok = from->kind == TY_FUNC || from->kind == TY_NULL ||
                           (from->kind == TY_POINTER &&
                            from->elem_type->kind == TY_VOID); break;
        case TY_STRUCT: ok = from->kind == TY_STRUCT &&
                             !strcmp(from->struct_name, to->struct_name); break;
        default: break;
    }
    if (!ok)
        report_error(s->filename, n->line, n->col, "cannot cast %s to %s",
                     type_str(s, from), type_str(s, to));
    return to;
}

static TypeInfo *expr(Sema *s, Node *n) {
    switch (n->kind) {
        case NK_INT_LIT:
            return n->type = n->int_lit.is_char ? &ty_char
                           : n->int_lit.is_unsigned ? &ty_u64 : &ty_int;
        case NK_FLOAT_LIT:  return n->type = &ty_float;
        case NK_STRING_LIT: return n->type = &ty_string;
        case NK_BOOL_LIT:   return n->type = &ty_bool;
        case NK_NULL_LIT:   return n->type = &ty_null;

        case NK_IDENT: {
            Symbol *y = lookup_cap(s, n->ident.name, &n->ident.capsym);
            if (y) y->used = true;
            if (!y) {
                suggest_symbol(s, n->ident.name, false);
                report_error(s->filename, n->line, n->col,
                             "undeclared identifier '%s'", n->ident.name);
                return n->type = &ty_unknown;
            }
            if (y->kind == SYM_FUNC) {            /* a function used as a value */
                n->ident.cname = y->cname;
                return n->type = func_value_type(s, y);
            }
            if (y->kind != SYM_VAR) {
                report_error(s->filename, n->line, n->col,
                             "'%s' is not a variable", n->ident.name);
                return n->type = &ty_unknown;
            }
            return n->type = y->type;
        }

        case NK_THIS:
            if (s->lam) {
                diag_hint("copy it first: edu me = ithu;  then use 'me' in the lambda");
                report_error(s->filename, n->line, n->col,
                             "a lambda cannot use 'ithu' directly");
                return n->type = &ty_unknown;
            }
            if (!s->cur_class || (s->cur_method && s->cur_method->is_static)) {
                report_error(s->filename, n->line, n->col,
                             "'ithu' can only be used inside a non-static method");
                return n->type = &ty_unknown;
            }
            return n->type = lookup_type(s, s->cur_class->class_decl.name)->type;

        case NK_SUPER:
            report_error(s->filename, n->line, n->col,
                         "'thandhai' must be followed by a method call, "
                         "e.g. thandhai.speak()");
            return n->type = &ty_unknown;

        case NK_CTOR_CALL: {
            Node *par = s->cur_class ? s->cur_class->class_decl.parent_decl : NULL;
            if (n != s->ctor_call || !par) {
                report_error(s->filename, n->line, n->col,
                             "thandhai(...) may only be the first statement of "
                             "a constructor of a class with a parent");
                expr_list(s, n->ctor_call.args);
                return n->type = &ty_void;
            }
            Method *pc = eff_ctor(par);
            n->ctor_call.ctor = pc;
            if (pc) check_args(s, pc->params, n->ctor_call.args,
                               par->class_decl.name, n->line, n->col);
            else {
                if (n->ctor_call.args)
                    report_error(s->filename, n->line, n->col,
                                 "the parent class has no constructor, so "
                                 "thandhai() takes no arguments");
                expr_list(s, n->ctor_call.args);
            }
            return n->type = &ty_void;
        }

        case NK_NEW: {
            Symbol *y = lookup_type(s, n->new_expr.name);
            if (!y || y->kind != SYM_CLASS) {
                report_error(s->filename, n->line, n->col,
                             "'puthu' needs a class, but '%s' is %s",
                             n->new_expr.name, y ? "not a class" : "unknown");
                expr_list(s, n->new_expr.args);
                return n->type = &ty_unknown;
            }
            if (y->decl->class_decl.is_abstract)
                report_error(s->filename, n->line, n->col,
                             "cannot create an object of the abstract class '%s'",
                             n->new_expr.name);
            Method *c = y->decl->class_decl.ctor;
            n->new_expr.ctor = c;
            if (c) {
                check_access(s, c->access, y->decl, "constructor of",
                             n->new_expr.name, n->line, n->col);
                check_args(s, c->params, n->new_expr.args, n->new_expr.name,
                           n->line, n->col);
            } else {
                if (n->new_expr.args)
                    report_error(s->filename, n->line, n->col,
                                 "class '%s' has no constructor, so it takes "
                                 "no arguments", n->new_expr.name);
                expr_list(s, n->new_expr.args);
            }
            return n->type = y->type;
        }

        case NK_LIST_NEW: {
            TypeInfo *lt = n->list_new.ltype;
            resolve_type(s, lt, n->line, n->col);
            if (lt->kind == TY_CLOSURE) {
                report_error(s->filename, n->line, n->col,
                             "Fn<...> is a type, not a class: write a lambda: seyal(x: int): int { ... }");
                return n->type = &ty_unknown;
            }
            if (lt->kind == TY_MAP) {
                if (!lt->is_set && n->list_new.items)
                    report_error(s->filename, n->line, n->col,
                                 "a Map is created empty: puthu Map<K, V>(); then use put() or m[k] = v");
                int i = 1;
                for (NodeList *a = n->list_new.items; lt->is_set && a; a = a->next, i++) {
                    TypeInfo *at = expr(s, a->node);
                    if (!assignable(s, lt->key_type, at, a->node))
                        report_error(s->filename, a->node->line, a->node->col,
                                     "set item %d: expected %s but got %s", i,
                                     type_str(s, lt->key_type), type_str(s, at));
                }
                return n->type = lt;
            }
            if (!is_unknown(lt->elem_type) && lt->elem_type->kind == TY_VOID)
                report_error(s->filename, n->line, n->col, "List<void> is not allowed");
            if (!is_unknown(lt->elem_type) && lt->elem_type->kind == TY_ARRAY)
                report_error(s->filename, n->line, n->col,
                             "List elements cannot be arrays; use a List of structs or a List<List<T>>");
            int i = 1;
            for (NodeList *a = n->list_new.items; a; a = a->next, i++) {
                TypeInfo *at = expr(s, a->node);
                if (!assignable(s, lt->elem_type, at, a->node))
                    report_error(s->filename, a->node->line, a->node->col,
                                 "list item %d: expected %s but got %s%s", i,
                                 type_str(s, lt->elem_type), type_str(s, at),
                                 conv_hint(s, lt->elem_type, at));
            }
            return n->type = lt;
        }

        case NK_ASSIGN: {
            TypeInfo *lt = expr(s, n->assign.target);
            TypeInfo *rt = expr(s, n->assign.value);
            check_modifiable(s, n->assign.target);
            int op = n->assign.op;
            if (is_unknown(lt) || is_unknown(rt)) return n->type = lt;
            if (lt->kind == TY_ARRAY) {
                report_error(s->filename, n->line, n->col,
                             "arrays cannot be assigned as a whole");
                return n->type = lt;
            }
            if (op == BOP_NONE) {
                if (!assignable(s, lt, rt, n->assign.value))
                    report_error(s->filename, n->line, n->col,
                                 "cannot assign %s to %s%s", type_str(s, rt),
                                 type_str(s, lt), conv_hint(s, lt, rt));
                return n->type = lt;
            }
            bool intonly = op == BOP_MOD || op == BOP_BAND || op == BOP_BOR ||
                           op == BOP_BXOR || op == BOP_SHL || op == BOP_SHR;
            if (op == BOP_ADD && lt->kind == TY_STRING && rt->kind == TY_STRING)
                return n->type = lt;
            if ((op == BOP_ADD || op == BOP_SUB) && lt->kind == TY_POINTER &&
                is_integral(rt))
                return n->type = lt;
            if (!is_arith(lt) || !is_arith(rt) || lt->kind == TY_ENUM ||
                (intonly && (!is_integral(lt) || !is_integral(rt))))
                report_error(s->filename, n->line, n->col,
                             "invalid operands to '%s=': %s and %s",
                             binop_name((BinOp)op), type_str(s, lt), type_str(s, rt));
            long long cv;
            if ((op == BOP_DIV || op == BOP_MOD) &&
                const_eval(n->assign.value, &cv) && cv == 0)
                report_error(s->filename, n->assign.value->line,
                             n->assign.value->col, "division by zero");
            return n->type = lt;
        }

        case NK_BINARY: return n->type = binary_expr(s, n);
        case NK_UNARY:  return n->type = unary_expr(s, n);
        case NK_CAST:   return n->type = cast_expr(s, n);

        case NK_TERNARY: {
            cond_check(s, n->ternary.cond, "a '?' condition");
            TypeInfo *a = expr(s, n->ternary.then_e);
            TypeInfo *b = expr(s, n->ternary.else_e);
            if (is_unknown(a) || is_unknown(b)) return n->type = &ty_unknown;
            if (is_arith(a) && is_arith(b))
                return n->type = (a->kind == TY_BOOL && b->kind == TY_BOOL)
                                 ? &ty_bool : arith_result(a, b);
            if (assignable(s, a, b, n->ternary.else_e)) return n->type = a;
            if (assignable(s, b, a, n->ternary.then_e)) return n->type = b;
            report_error(s->filename, n->line, n->col,
                         "the two branches of '?:' have incompatible types "
                         "%s and %s", type_str(s, a), type_str(s, b));
            return n->type = &ty_unknown;
        }

        case NK_INCDEC: {
            TypeInfo *t = expr(s, n->incdec.operand);
            if (!is_lvalue(n->incdec.operand)) {
                report_error(s->filename, n->line, n->col,
                             "'%s' needs a variable to modify",
                             n->incdec.is_inc ? "++" : "--");
                return n->type = t;
            }
            check_modifiable(s, n->incdec.operand);
            if (!is_unknown(t) && t->kind != TY_INT && t->kind != TY_FLOAT &&
                t->kind != TY_POINTER)
                report_error(s->filename, n->line, n->col,
                             "'%s' cannot be applied to %s",
                             n->incdec.is_inc ? "++" : "--", type_str(s, t));
            return n->type = t;
        }

        case NK_SIZEOF: {
            if (n->size_of.expr && n->size_of.expr->kind == NK_IDENT) {
                Symbol *y = lookup_type(s, n->size_of.expr->ident.name);
                Symbol *v = lookup(s, n->size_of.expr->ident.name);
                if (y && (!v || v == y)) {        /* alavu(TypeName) */
                    n->size_of.of_type = type_new(s->arena, TY_STRUCT);
                    n->size_of.of_type->struct_name = n->size_of.expr->ident.name;
                    n->size_of.expr = NULL;
                }
            }
            if (n->size_of.of_type) {
                resolve_type(s, n->size_of.of_type, n->line, n->col);
                if (n->size_of.of_type->kind == TY_VOID)
                    report_error(s->filename, n->line, n->col,
                                 "alavu(void) is not allowed");
            } else {
                TypeInfo *t = expr(s, n->size_of.expr);
                if (!is_unknown(t) && t->kind == TY_VOID)
                    report_error(s->filename, n->line, n->col,
                                 "alavu of a void value");
            }
            return n->type = &ty_usize;
        }

        case NK_STRUCT_LIT: {
            Symbol *y = lookup_type(s, n->struct_lit.name);
            if (!y || y->kind != SYM_STRUCT) {
                report_error(s->filename, n->line, n->col,
                             y && y->kind == SYM_CLASS
                                 ? "'%s' is a class; create it with puthu %s(...)"
                                 : "'%s' is not a struct or union",
                             n->struct_lit.name, n->struct_lit.name);
                for (FieldInit *fi = n->struct_lit.inits; fi; fi = fi->next)
                    expr(s, fi->value);
                return n->type = &ty_unknown;
            }
            int count = 0;
            for (FieldInit *fi = n->struct_lit.inits; fi; fi = fi->next, count++) {
                TypeInfo *vt = expr(s, fi->value);
                Field *f = find_field(y->fields, fi->name);
                if (!f) {
                    report_error(s->filename, fi->line, fi->col,
                                 "%s '%s' has no field '%s'",
                                 y->decl->struct_decl.is_union ? "union" : "struct",
                                 n->struct_lit.name, fi->name);
                    continue;
                }
                for (FieldInit *g = n->struct_lit.inits; g != fi; g = g->next)
                    if (!strcmp(g->name, fi->name))
                        report_error(s->filename, fi->line, fi->col,
                                     "field '%s' is initialised twice", fi->name);
                if (f->type->kind == TY_ARRAY)
                    report_error(s->filename, fi->line, fi->col,
                                 "array field '%s' cannot be set in a struct "
                                 "literal; assign its elements afterwards", fi->name);
                else if (!assignable(s, f->type, vt, fi->value))
                    report_error(s->filename, fi->line, fi->col,
                                 "field '%s' (%s) cannot be initialised with %s%s",
                                 fi->name, type_str(s, f->type), type_str(s, vt),
                                 conv_hint(s, f->type, vt));
            }
            if (y->decl->struct_decl.is_union && count > 1)
                report_error(s->filename, n->line, n->col,
                             "a union literal can set only one field");
            return n->type = y->type;
        }

        case NK_CALL: {
            Symbol *y = lookup_cap(s, n->call.name, &n->call.capsym);
            if (y) y->used = true;
            if (!y && !strcmp(n->call.name, "str")) {       /* built-in: str(value [, "format"]) */
                int argc = 0;
                for (NodeList *a = n->call.args; a; a = a->next) argc++;
                if (argc < 1 || argc > 2) {
                    report_error(s->filename, n->line, n->col,
                                 "str(value) or str(value, \"format\") takes 1 or 2 arguments");
                    expr_list(s, n->call.args);
                    return n->type = &ty_string;
                }
                TypeInfo *vt = expr(s, n->call.args->node);
                if (!is_unknown(vt) && vt->kind != TY_INT && vt->kind != TY_FLOAT &&
                    vt->kind != TY_BOOL && vt->kind != TY_STRING && vt->kind != TY_ENUM)
                    report_error(s->filename, n->call.args->node->line, n->call.args->node->col,
                                 "str() converts numbers, bool, enum and string, not %s",
                                 type_str(s, vt));
                if (argc == 2) {
                    Node *f = n->call.args->next->node;
                    expr(s, f);
                    if (f->kind != NK_STRING_LIT) {
                        report_error(s->filename, f->line, f->col,
                                     "the format of str() must be a string literal such as \".2f\" or \"05d\"");
                    } else {
                        const char *sp = f->string_lit.value;
                        size_t k = 0, len = strlen(sp);
                        while (k < len && strchr("-+ #0123456789.", sp[k])) k++;
                        if (k < len - (len ? 1 : 0) || (k < len && !strchr("dioxXuceEfgGs", sp[k])))
                            report_error(s->filename, f->line, f->col,
                                         "bad format \"%s\": use flags, width, .precision and one of "
                                         "d x X o u c f e g s (for example \"08.3f\")", sp);
                    }
                }
                n->call.builtin = 1;
                return n->type = &ty_string;
            }
            if (!y) {
                suggest_symbol(s, n->call.name, true);
                report_error(s->filename, n->line, n->col,
                             "call to undeclared function '%s'", n->call.name);
                expr_list(s, n->call.args);
                return n->type = &ty_unknown;
            }
            if (y->kind == SYM_VAR && !is_unknown(y->type) && y->type->kind == TY_CLOSURE)
                n->call.is_closure = true;
            if (y->kind == SYM_VAR && !is_unknown(y->type) &&
                (y->type->kind == TY_FUNC || y->type->kind == TY_CLOSURE)) {
                check_args(s, y->type->fparams, n->call.args, n->call.name,
                           n->line, n->col);
                n->call.params = y->type->fparams;
                n->call.cname = NULL;
                return n->type = y->type->fret;
            }
            if (y->kind != SYM_FUNC) {
                report_error(s->filename, n->line, n->col,
                             y->kind == SYM_CLASS
                                 ? "'%s' is a class; create objects with puthu %s(...)"
                                 : "'%s' is not a function",
                             n->call.name, n->call.name);
                expr_list(s, n->call.args);
                return n->type = &ty_unknown;
            }
            check_args(s, y->params, n->call.args, n->call.name, n->line, n->col);
            n->call.params = y->params;
            n->call.cname = y->cname;          /* non-NULL: call into C */
            return n->type = y->type;
        }

        case NK_LAMBDA: {
            Node *fn = n->lambda.fn;
            for (Param *p = fn->func_decl.params; p; p = p->next)
                resolve_type(s, p->type, fn->line, fn->col);
            resolve_type(s, fn->func_decl.ret_type, fn->line, fn->col);
            TypeInfo *rt = fn->func_decl.ret_type ? fn->func_decl.ret_type : &ty_void;
            LambdaCtx ctx = {0};
            ctx.outer = s->lam;
            s->lam = &ctx;
            int saved_loops = s->loop_depth;
            s->loop_depth = 0;
            func_decl(s, fn);
            s->loop_depth = saved_loops;
            s->lam = ctx.outer;
            for (Capture *c = ctx.caps; c; c = c->next)
                if (!is_unknown(c->type) && c->type->kind == TY_ARRAY)
                    report_error(s->filename, n->line, n->col,
                                 "a lambda cannot capture the array '%s' (capture a List instead)",
                                 c->name);
            n->lambda.caps = ctx.caps;
            n->lambda.ncap = ctx.ncap;
            TypeInfo *ft = type_func(s->arena, fn->func_decl.params, rt);
            if (ctx.ncap) ft->kind = TY_CLOSURE;
            return n->type = ft;
        }

        case NK_MCALL:  return n->type = mcall_expr(s, n);

        case NK_INDEX: {
            TypeInfo *at = expr(s, n->index.array);
            TypeInfo *it = expr(s, n->index.index);
            if (!is_unknown(at) && at->kind == TY_MAP) {
                if (at->is_set) {
                    report_error(s->filename, n->line, n->col,
                                 "a Set has no values to index; use has(), add() and remove()");
                    return n->type = &ty_unknown;
                }
                if (!assignable(s, at->key_type, it, n->index.index))
                    report_error(s->filename, n->index.index->line, n->index.index->col,
                                 "this %s needs %s keys, not %s", type_str(s, at),
                                 type_str(s, at->key_type), type_str(s, it));
                return n->type = at->elem_type;
            }
            if (!is_integral(it))
                report_error(s->filename, n->index.index->line,
                             n->index.index->col,
                             "array index must be an integer, not %s",
                             type_str(s, it));
            if (is_unknown(at)) return n->type = &ty_unknown;
            if (at->kind == TY_LIST) return n->type = at->elem_type;
            if (at->kind != TY_ARRAY && at->kind != TY_POINTER &&
                at->kind != TY_STRING) {
                report_error(s->filename, n->line, n->col,
                             "cannot index a value of type %s", type_str(s, at));
                return n->type = &ty_unknown;
            }
            long long v;
            if (at->kind == TY_ARRAY && const_eval(n->index.index, &v) &&
                (v < 0 || (at->array_size > 0 && v >= at->array_size)))
                report_error(s->filename, n->index.index->line,
                             n->index.index->col,
                             "index %lld is out of bounds for %s", v,
                             type_str(s, at));
            if (at->kind == TY_STRING) return n->type = &ty_char;
            if (at->elem_type->kind == TY_VOID) {
                report_error(s->filename, n->line, n->col,
                             "cannot index a void*; cast it with maatru first");
                return n->type = &ty_unknown;
            }
            return n->type = at->elem_type;
        }

        case NK_MEMBER: return n->type = member_expr(s, n);

        case NK_ARRAY_INIT:
            report_error(s->filename, n->line, n->col,
                         "an array literal may only initialise an 'edu' "
                         "declaration");
            expr_list(s, n->array_init.elements);
            return n->type = &ty_unknown;

        default:
            return n->type = &ty_unknown;
    }
}

/* obj.field, Enum.Member, Class.static_field */
static TypeInfo *member_expr(Sema *s, Node *n) {
    Node *obj = n->member.object;
    const char *fname = n->member.field;

    if (obj->kind == NK_IDENT) {
        Symbol *y = lookup(s, obj->ident.name);
        if (y && y->kind == SYM_ENUM) {
            enum_resolve(s, y);
            for (Enumerator *e = y->enums; e; e = e->next)
                if (!strcmp(e->name, fname)) {
                    if (!e->done) {
                        report_error(s->filename, n->line, n->col,
                                     "enumerator '%s.%s' is used before its "
                                     "value is known", y->name, fname);
                        return &ty_int;
                    }
                    n->kind = NK_INT_LIT;              /* fold to its value */
                    n->int_lit.value = e->resolved;
                    n->int_lit.is_unsigned = false;
                    n->int_lit.is_char = false;
                    return y->type;
                }
            report_error(s->filename, n->line, n->col,
                         "enum '%s' has no member '%s'", y->name, fname);
            return &ty_unknown;
        }
        if (y && y->kind == SYM_CLASS) {
            Node *owner = NULL;
            Field *f = find_cfield(y->decl, fname, &owner, NULL);
            if (!f || !f->is_static) {
                report_error(s->filename, n->line, n->col,
                             f ? "'%s' is an instance field; use an object, not "
                                 "the class name"
                               : "class '%s' has no static field '%s'",
                             f ? fname : y->name, fname);
                return &ty_unknown;
            }
            check_access(s, f->access, owner, "field", fname, n->line, n->col);
            n->member.is_static = true;
            n->member.owner = owner->class_decl.name;
            return f->type;
        }
        if (y && y->kind != SYM_VAR) {
            report_error(s->filename, n->line, n->col,
                         "'%s' is not a variable", obj->ident.name);
            return &ty_unknown;
        }
    }

    TypeInfo *ot = expr(s, obj);
    if (is_unknown(ot)) return &ty_unknown;

    if (!strcmp(fname, "length") &&
        (ot->kind == TY_ARRAY || ot->kind == TY_STRING || ot->kind == TY_LIST ||
         ot->kind == TY_MAP)) {
        if (ot->kind == TY_ARRAY) {
            if (ot->array_size <= 0) {
                report_error(s->filename, n->line, n->col,
                             "this array has no known length (it was passed as T[])");
                return &ty_int;
            }
            n->kind = NK_INT_LIT;                 /* a compile-time constant */
            n->int_lit.value = ot->array_size;
            n->int_lit.is_unsigned = false;
            n->int_lit.is_char = false;
        } else {
            n->member.hops = ot->kind == TY_STRING ? -2 : ot->kind == TY_MAP ? -4 : -3;
        }
        return &ty_int;
    }

    if (ot->kind == TY_CLASS) {
        Node *cls = class_of_type(s, ot), *owner = NULL;
        int hops = 0;
        Field *f = cls ? find_cfield(cls, fname, &owner, &hops) : NULL;
        if (!f) {
            if (cls && find_method(cls, fname, NULL))
                report_error(s->filename, n->line, n->col,
                             "'%s' is a method of '%s'; call it with ()",
                             fname, ot->struct_name);
            else
            {
                suggest_members(cls, fname, false);
                report_error(s->filename, n->line, n->col,
                             "class '%s' has no field '%s'", ot->struct_name, fname);
            }
            return &ty_unknown;
        }
        check_access(s, f->access, owner, "field", fname, n->line, n->col);
        n->member.hops = hops;
        n->member.owner = owner->class_decl.name;
        n->member.is_static = f->is_static;
        return f->type;
    }

    TypeInfo *st = ot;
    if (ot->kind == TY_POINTER && ot->elem_type->kind == TY_STRUCT) st = ot->elem_type;
    if (st->kind != TY_STRUCT) {
        report_error(s->filename, n->line, n->col,
                     "'.' applied to %s, which has no fields", type_str(s, ot));
        return &ty_unknown;
    }
    Symbol *sd = lookup_type(s, st->struct_name);
    if (!sd || sd->kind != SYM_STRUCT) return &ty_unknown;
    Field *f = find_field(sd->fields, fname);
    if (!f) {
        suggest_fields(sd->fields, fname);
        report_error(s->filename, n->line, n->col, "%s '%s' has no field '%s'",
                     sd->decl->struct_decl.is_union ? "union" : "struct",
                     st->struct_name, fname);
        return &ty_unknown;
    }
    return f->type;
}

/* obj.method(args), Class.static_method(args), thandhai.method(args),
 * struct.callback(args) */
static TypeInfo *mcall_expr(Sema *s, Node *n) {
    Node *obj = n->mcall.object;
    const char *mname = n->mcall.method;
    Node *cls = NULL;

    if (obj->kind == NK_IDENT) {
        Symbol *y = lookup(s, obj->ident.name);
        if (y && y->kind == SYM_CLASS) {
            Node *owner = NULL;
            Method *m = find_method(y->decl, mname, &owner);
            if (!m || !m->is_static) {
                report_error(s->filename, n->line, n->col,
                             m ? "'%s' is an instance method; call it on an object"
                               : "class '%s' has no static method '%s'",
                             m ? mname : y->name, mname);
                expr_list(s, n->mcall.args);
                return &ty_unknown;
            }
            check_access(s, m->access, owner, "method", mname, n->line, n->col);
            check_args(s, m->params, n->mcall.args, mname, n->line, n->col);
            n->mcall.resolved = m;
            n->mcall.is_static_call = true;
            return m->ret_type ? m->ret_type : &ty_void;
        }
        if (y && y->kind == SYM_ENUM) {
            report_error(s->filename, n->line, n->col,
                         "enum '%s' has no methods", y->name);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
    }

    if (obj->kind == NK_SUPER) {
        Node *par = s->cur_class ? s->cur_class->class_decl.parent_decl : NULL;
        if (!par || !s->cur_method || s->cur_method->is_static) {
            report_error(s->filename, obj->line, obj->col,
                         "'thandhai' needs a parent class and a non-static method");
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        Node *owner = NULL;
        Method *m = find_method(par, mname, &owner);
        if (!m || m->is_static) {
            report_error(s->filename, n->line, n->col,
                         "parent class '%s' has no method '%s'",
                         par->class_decl.name, mname);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        check_access(s, m->access, owner, "method", mname, n->line, n->col);
        if (m->is_abstract)
            report_error(s->filename, n->line, n->col,
                         "cannot call the abstract method '%s' of the parent", mname);
        check_args(s, m->params, n->mcall.args, mname, n->line, n->col);
        n->mcall.resolved = m;
        n->mcall.is_super_call = true;
        obj->type = lookup_type(s, s->cur_class->class_decl.name)->type;
        return m->ret_type ? m->ret_type : &ty_void;
    }

    TypeInfo *ot = expr(s, obj);
    if (is_unknown(ot)) { expr_list(s, n->mcall.args); return &ty_unknown; }

    if (ot->kind == TY_LIST) {
        if (!strcmp(mname, "join") && !is_unknown(ot->elem_type) && ot->elem_type->kind == TY_STRING) {
            int argc = 0;
            for (NodeList *a = n->mcall.args; a; a = a->next) argc++;
            if (argc != 1) {
                report_error(s->filename, n->line, n->col, "List.join takes 1 argument (the separator)");
                expr_list(s, n->mcall.args);
                return &ty_string;
            }
            TypeInfo *at = expr(s, n->mcall.args->node);
            if (!assignable(s, &ty_string, at, n->mcall.args->node))
                report_error(s->filename, n->mcall.args->node->line, n->mcall.args->node->col,
                             "the separator must be a string, not %s", type_str(s, at));
            n->mcall.builtin = 8;
            return &ty_string;
        }
        static const struct { const char *name; int argc; int id; } LM[] = {
            { "push", 1, 1 }, { "pop", 0, 2 }, { "get", 1, 3 }, { "set", 2, 4 },
            { "insert", 2, 5 }, { "remove", 1, 6 }, { "clear", 0, 7 },
        };
        int k = -1;
        for (size_t i = 0; i < sizeof LM / sizeof LM[0]; i++)
            if (!strcmp(LM[i].name, mname)) k = (int)i;
        if (k < 0) {
            report_error(s->filename, n->line, n->col,
                         "List has no method '%s' (push pop get set insert remove clear, "
                         "join for List<string>, and .length)", mname);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        int argc = 0;
        for (NodeList *a = n->mcall.args; a; a = a->next) argc++;
        if (argc != LM[k].argc) {
            report_error(s->filename, n->line, n->col,
                         "List.%s takes %d argument(s), not %d", mname, LM[k].argc, argc);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        int id = LM[k].id;
        NodeList *a1 = n->mcall.args, *a2 = a1 ? a1->next : NULL;
        bool idx_first = id == 3 || id == 4 || id == 5 || id == 6;
        bool has_val = id == 1 || id == 4 || id == 5;
        Node *idx = idx_first ? a1->node : NULL;
        Node *val = id == 1 ? a1->node : (id == 4 || id == 5) ? a2->node : NULL;
        if (idx) {
            TypeInfo *it = expr(s, idx);
            if (!is_integral(it))
                report_error(s->filename, idx->line, idx->col,
                             "list index must be an integer, not %s", type_str(s, it));
        }
        if (has_val) {
            TypeInfo *vt = expr(s, val);
            if (!assignable(s, ot->elem_type, vt, val))
                report_error(s->filename, val->line, val->col,
                             "cannot store %s in a %s%s", type_str(s, vt),
                             type_str(s, ot), conv_hint(s, ot->elem_type, vt));
        }
        n->mcall.builtin = id;
        return (id == 2 || id == 3 || id == 6) ? ot->elem_type : &ty_void;
    }

    if (ot->kind == TY_STRING) {
        static const struct { const char *name; int argc; int id; const char *args; } SM[] = {
            { "upper", 0, 41, "" },   { "lower", 0, 42, "" },   { "trim", 0, 43, "" },
            { "contains", 1, 44, "s" }, { "starts", 1, 45, "s" }, { "ends", 1, 46, "s" },
            { "find", 1, 47, "s" },   { "sub", 2, 48, "ii" },   { "replace", 2, 49, "ss" },
            { "split", 1, 50, "s" },  { "repeat", 1, 51, "i" }, { "to_int", 0, 52, "" },
            { "to_float", 0, 53, "" },
        };
        int k = -1;
        for (size_t i = 0; i < sizeof SM / sizeof SM[0]; i++)
            if (!strcmp(SM[i].name, mname)) k = (int)i;
        if (k < 0) {
            report_error(s->filename, n->line, n->col,
                         "string has no method '%s' (upper lower trim contains starts ends find sub "
                         "replace split repeat to_int to_float, and .length)", mname);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        int argc = 0;
        for (NodeList *a = n->mcall.args; a; a = a->next) argc++;
        if (argc != SM[k].argc) {
            report_error(s->filename, n->line, n->col, "string.%s takes %d argument(s), not %d",
                         mname, SM[k].argc, argc);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        int ai = 0;
        for (NodeList *a = n->mcall.args; a; a = a->next, ai++) {
            TypeInfo *at = expr(s, a->node);
            TypeInfo *want = SM[k].args[ai] == 's' ? &ty_string : &ty_int;
            if (!assignable(s, want, at, a->node))
                report_error(s->filename, a->node->line, a->node->col,
                             "argument %d of string.%s: expected %s but got %s", ai + 1, mname,
                             type_str(s, want), type_str(s, at));
        }
        n->mcall.builtin = SM[k].id;
        switch (SM[k].id) {
            case 44: case 45: case 46: return &ty_bool;
            case 47: case 52: return &ty_int;
            case 53: return &ty_float;
            case 50: {
                TypeInfo *lt = type_new(s->arena, TY_LIST);
                lt->elem_type = &ty_string;
                return lt;
            }
            default: return &ty_string;
        }
    }

    if (ot->kind == TY_MAP) {
        static const struct { const char *name; int argc; int id; bool set; } MM[] = {
            { "put", 2, 11, false }, { "get", 1, 12, false }, { "get_or", 2, 13, false },
            { "has", 1, 14, false }, { "remove", 1, 15, false }, { "clear", 0, 16, false },
            { "keys", 0, 17, false }, { "values", 0, 18, false },
            { "add", 1, 21, true }, { "has", 1, 22, true }, { "remove", 1, 23, true },
            { "clear", 0, 24, true }, { "items", 0, 25, true },
        };
        int k = -1;
        for (size_t i = 0; i < sizeof MM / sizeof MM[0]; i++)
            if (MM[i].set == ot->is_set && !strcmp(MM[i].name, mname)) k = (int)i;
        if (k < 0) {
            report_error(s->filename, n->line, n->col, ot->is_set
                         ? "Set has no method '%s' (add has remove clear items, and .length)"
                         : "Map has no method '%s' (put get get_or has remove clear keys values, "
                           "and .length)", mname);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        int argc = 0;
        for (NodeList *a = n->mcall.args; a; a = a->next) argc++;
        if (argc != MM[k].argc) {
            report_error(s->filename, n->line, n->col, "%s.%s takes %d argument(s), not %d",
                         ot->is_set ? "Set" : "Map", mname, MM[k].argc, argc);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        int id = MM[k].id;
        NodeList *a1 = n->mcall.args, *a2 = a1 ? a1->next : NULL;
        if (a1) {
            TypeInfo *kt = expr(s, a1->node);
            if (!assignable(s, ot->key_type, kt, a1->node))
                report_error(s->filename, a1->node->line, a1->node->col,
                             "%s.%s needs a %s key, not %s", ot->is_set ? "Set" : "Map",
                             mname, type_str(s, ot->key_type), type_str(s, kt));
        }
        if (a2) {
            TypeInfo *vt = expr(s, a2->node);
            if (!assignable(s, ot->elem_type, vt, a2->node))
                report_error(s->filename, a2->node->line, a2->node->col,
                             "cannot store %s in a %s", type_str(s, vt), type_str(s, ot));
        }
        n->mcall.builtin = id;
        switch (id) {
            case 12: case 13: return ot->elem_type;
            case 14: case 15: case 22: case 23: return &ty_bool;
            case 21: return &ty_bool;
            case 17: case 25: case 18: {
                TypeInfo *lt = type_new(s->arena, TY_LIST);
                lt->elem_type = id == 18 ? ot->elem_type : ot->key_type;
                return lt;
            }
            default: return &ty_void;
        }
    }

    if (ot->kind == TY_CLASS) {
        cls = class_of_type(s, ot);
        Node *owner = NULL;
        Method *m = cls ? find_method(cls, mname, &owner) : NULL;
        if (!m) {
            suggest_members(cls, mname, true);
            report_error(s->filename, n->line, n->col,
                         "class '%s' has no method '%s'", ot->struct_name, mname);
            expr_list(s, n->mcall.args);
            return &ty_unknown;
        }
        check_access(s, m->access, owner, "method", mname, n->line, n->col);
        check_args(s, m->params, n->mcall.args, mname, n->line, n->col);
        n->mcall.resolved = m;
        n->mcall.is_static_call = m->is_static;
        return m->ret_type ? m->ret_type : &ty_void;
    }

    /* a function-pointer field of a struct */
    TypeInfo *st = ot;
    if (ot->kind == TY_POINTER && ot->elem_type->kind == TY_STRUCT) st = ot->elem_type;
    if (st->kind == TY_STRUCT) {
        Symbol *sd = lookup_type(s, st->struct_name);
        Field *f = sd && sd->kind == SYM_STRUCT ? find_field(sd->fields, mname) : NULL;
        if (f && (f->type->kind == TY_FUNC || f->type->kind == TY_CLOSURE)) {
            check_args(s, f->type->fparams, n->mcall.args, mname, n->line, n->col);
            n->mcall.fparams = f->type->fparams;
            n->mcall.closure_field = f->type->kind == TY_CLOSURE;
            return f->type->fret;
        }
    }
    report_error(s->filename, n->line, n->col,
                 "cannot call '%s' on a value of type %s", mname, type_str(s, ot));
    expr_list(s, n->mcall.args);
    return &ty_unknown;
}

/* ─── Statements and declarations ──────────────────────────────────────── */
static void var_decl(Sema *s, Node *n) {
    TypeInfo *vt   = n->var_decl.var_type;
    Node     *init = n->var_decl.init;
    const char *name = n->var_decl.name;
    int quals = n->var_decl.quals;
    bool global = s->scope == s->global;

    if (vt) {
        resolve_type(s, vt, n->line, n->col);
        if (vt->kind == TY_VOID)
            report_error(s->filename, n->line, n->col,
                         "variable '%s' cannot have type void", name);
    }
    if ((quals & Q_CONST) && !init)
        report_error(s->filename, n->line, n->col,
                     "nilai variable '%s' needs an initialiser", name);

    if (init && init->kind == NK_ARRAY_INIT) {
        TypeInfo *et = NULL;
        if (vt && vt->kind != TY_ARRAY && !is_unknown(vt))
            report_error(s->filename, n->line, n->col,
                         "cannot initialise %s with an array literal",
                         type_str(s, vt));
        else if (vt && vt->kind == TY_ARRAY)
            et = vt->elem_type;

        int count = 0;
        TypeInfo *first = NULL;
        for (NodeList *e = init->array_init.elements; e; e = e->next, count++) {
            TypeInfo *t = expr(s, e->node);
            if (!first) first = t;
            TypeInfo *want = et ? et : first;
            if (want && !(et ? assignable(s, want, t, e->node)
                             : (assignable(s, want, t, e->node) ||
                                assignable(s, t, want, NULL))))
                report_error(s->filename, e->node->line, e->node->col,
                             et ? "array element of type %s does not match %s"
                                : "array elements must all have the same "
                                  "type (%s vs %s)",
                             type_str(s, t), type_str(s, want));
        }
        if (!vt) {
            vt = type_array(s->arena, first ? first : &ty_unknown, count);
        } else if (vt->kind == TY_ARRAY) {
            if (vt->array_size < 0) vt->array_size = count;
            else if (count > vt->array_size)
                report_error(s->filename, init->line, init->col,
                             "too many initialisers: %d for %s",
                             count, type_str(s, vt));
        }
        if (vt->kind == TY_ARRAY && vt->array_size <= 0)
            report_error(s->filename, n->line, n->col,
                         "an empty array literal needs an explicit size, "
                         "e.g. int[8]");
        init->type = vt;
    } else if (init) {
        TypeInfo *it = expr(s, init);
        if (!vt) {
            vt = it;
            if (!is_unknown(vt) && vt->kind == TY_VOID)
                report_error(s->filename, n->line, n->col,
                             "cannot initialise '%s' from a void value", name);
            else if (!is_unknown(vt) && vt->kind == TY_NULL) {
                report_error(s->filename, n->line, n->col,
                             "cannot infer the type of '%s' from illai; "
                             "add a type", name);
                vt = &ty_unknown;
            }
        } else if (!assignable(s, vt, it, init)) {
            long long cv;
            if (is_integral(vt) && vt->kind == TY_INT && const_eval(init, &cv))
                report_error(s->filename, n->line, n->col,
                             "the value %lld does not fit in %s (variable '%s')",
                             cv, type_str(s, vt), name);
            else
                report_error(s->filename, n->line, n->col,
                             "cannot initialise '%s' (%s) with a value of type %s%s",
                             name, type_str(s, vt), type_str(s, it),
                             conv_hint(s, vt, it));
        }
        if (!is_unknown(vt) && vt->kind == TY_ARRAY)
            report_error(s->filename, n->line, n->col,
                         "arrays cannot be copied; initialise '%s' with an "
                         "array literal", name);
    } else {
        if (!vt)
            report_error(s->filename, n->line, n->col,
                         "cannot infer the type of '%s'; add a type or an "
                         "initialiser", name);
        else if (vt->kind == TY_ARRAY && vt->array_size <= 0)
            report_error(s->filename, n->line, n->col,
                         "array '%s' needs a size or an initialiser", name);
    }

    if ((quals & Q_STATIC) && init && !is_const_expr(init)) {
        bool ok = false;
        if (global) ok = true;           /* globals may be set at start-up */
        if (!ok)
            report_error(s->filename, n->line, n->col,
                         "the initialiser of the thangu (static) variable '%s' "
                         "must be a constant", name);
    }

    n->type = vt ? vt : &ty_unknown;
    Symbol *y = declare(s, name, SYM_VAR, n->type, n->line, n->col);
    if (y) {
        y->is_const = (quals & Q_CONST) != 0;
        y->track = !global;
        y->line = n->line; y->col = n->col;
    }
}

static void func_header(Sema *s, TypeInfo *rt, Param *params, int line, int col) {
    resolve_type(s, rt, line, col);
    if (rt->kind == TY_ARRAY)
        report_error(s->filename, line, col, "functions cannot return arrays");
    for (Param *p = params; p; p = p->next)
        if (p->type) {
            resolve_type(s, p->type, line, col);
            if (p->type->kind == TY_VOID)
                report_error(s->filename, line, col,
                             "parameter '%s' cannot have type void", p->name);
        }
}

/* ─── Control-flow analysis: does every path end in kudu / erivi? ─────── */
static bool has_break(Node *n) {                    /* a udai that leaves this statement */
    if (!n) return false;
    switch (n->kind) {
        case NK_BREAK: return true;
        case NK_BLOCK:
            for (NodeList *i = n->block.stmts; i; i = i->next) if (has_break(i->node)) return true;
            return false;
        case NK_IF:
            for (IfBranch *b = n->if_stmt.branches; b; b = b->next) if (has_break(b->body)) return true;
            return false;
        case NK_TRY: return has_break(n->try_stmt.body) || has_break(n->try_stmt.catch_body);
        default: return false;                        /* nested loops / switches own their breaks */
    }
}

static bool always_returns(Node *n) {
    if (!n) return false;
    switch (n->kind) {
        case NK_RETURN: case NK_THROW: return true;
        case NK_BLOCK:
            for (NodeList *i = n->block.stmts; i; i = i->next)
                if (always_returns(i->node)) return true;
            return false;
        case NK_IF: {
            bool has_else = false;
            for (IfBranch *b = n->if_stmt.branches; b; b = b->next) {
                if (!b->cond) has_else = true;
                if (!always_returns(b->body)) return false;
            }
            return has_else;
        }
        case NK_WHILE:
            return n->while_stmt.cond->kind == NK_BOOL_LIT && n->while_stmt.cond->bool_lit.value &&
                   !has_break(n->while_stmt.body);
        case NK_DO_WHILE:
            return always_returns(n->while_stmt.body);
        case NK_FOR:
            return !n->for_stmt.cond && !has_break(n->for_stmt.body);
        case NK_TRY: return always_returns(n->try_stmt.body) && always_returns(n->try_stmt.catch_body);
        case NK_SWITCH: {
            bool def = false;
            for (SwitchCase *c = n->switch_stmt.cases; c; c = c->next) {
                if (c->is_default) def = true;
                if (!always_returns(c->body) || has_break(c->body)) return false;
            }
            return def;
        }
        default: return false;
    }
}

static void check_returns(Sema *s, TypeInfo *rt, Node *body, int line, int col,
                          const char *what) {
    if (!rt || rt->kind == TY_VOID || !body || always_returns(body)) return;
    diag_hint("add a 'kudu' on every path, or make the return type void");
    report_error(s->filename, line, col,
                 "%s may reach its end without returning a value of type %s", what,
                 type_str(s, rt));
}

static void body_with_params(Sema *s, Param *params, TypeInfo *rt, Node *body,
                             int line, int col) {
    TypeInfo *saved = s->current_ret;
    s->current_ret = rt;
    scope_push(s);
    if (s->lam && !((LambdaCtx *)s->lam)->boundary) ((LambdaCtx *)s->lam)->boundary = s->scope;
    for (Param *p = params; p; p = p->next)
        declare(s, p->name, SYM_VAR, p->type ? p->type : &ty_unknown, line, col);
    stmt(s, body);
    scope_pop(s);
    s->current_ret = saved;
}

static void func_decl(Sema *s, Node *n) {
    TypeInfo *rt = n->func_decl.ret_type ? n->func_decl.ret_type : &ty_void;
    body_with_params(s, n->func_decl.params, rt, n->func_decl.body, n->line, n->col);
    if (strcmp(n->func_decl.name, "main") != 0)
        check_returns(s, rt, n->func_decl.body, n->line, n->col,
                      n->is_lambda ? "the lambda" : tfmt(s, "function '%s'", n->func_decl.name));
}

/* ─── Type declarations ─────────────────────────────────────────────────── */
static void struct_check(Sema *s, Node *n) {
    const char *kind = n->struct_decl.is_union ? "union" : "struct";
    for (Field *f = n->struct_decl.fields; f; f = f->next) {
        resolve_type(s, f->type, n->line, n->col);
        if (f->type->kind == TY_VOID)
            report_error(s->filename, n->line, n->col,
                         "field '%s' cannot have type void", f->name);
        if (f->type->kind == TY_ARRAY && f->type->array_size <= 0)
            report_error(s->filename, n->line, n->col,
                         "array field '%s' needs a size, e.g. int[4]", f->name);
        TypeInfo *b = f->type;
        while (b->kind == TY_ARRAY) b = b->elem_type;
        if (b->kind == TY_STRUCT && !strcmp(b->struct_name, n->struct_decl.name))
            report_error(s->filename, n->line, n->col,
                         "%s '%s' cannot contain itself (use a pointer)", kind,
                         n->struct_decl.name);
        for (Field *g = n->struct_decl.fields; g != f; g = g->next)
            if (!strcmp(g->name, f->name))
                report_error(s->filename, n->line, n->col,
                             "duplicate field '%s' in %s '%s'",
                             f->name, kind, n->struct_decl.name);
    }
}

static void enum_resolve(Sema *s, Symbol *y) {
    if (y->state != 0) return;
    y->state = 1;
    long long next = 0;
    for (Enumerator *e = y->enums; e; e = e->next) {
        if (e->value) {
            long long v;
            expr(s, e->value);
            if (const_eval(e->value, &v)) next = v;
            else report_error(s->filename, e->line, e->col,
                              "the value of enumerator '%s' must be a constant "
                              "integer", e->name);
        }
        e->resolved = next++;
        e->done = true;
        for (Enumerator *g = y->enums; g != e; g = g->next)
            if (!strcmp(g->name, e->name))
                report_error(s->filename, e->line, e->col,
                             "duplicate enumerator '%s' in enum '%s'",
                             e->name, y->name);
    }
    y->state = 2;
}

static bool same_sig(Method *a, Method *b) {
    Param *x = a->params, *y = b->params;
    for (; x && y; x = x->next, y = y->next) {
        TypeInfo *tx = x->type ? x->type : &ty_unknown;
        TypeInfo *ty = y->type ? y->type : &ty_unknown;
        if (tx->kind != TY_UNKNOWN && ty->kind != TY_UNKNOWN && !type_same(tx, ty))
            return false;
    }
    if (x || y) return false;
    return type_same(a->ret_type ? a->ret_type : &ty_void,
                     b->ret_type ? b->ret_type : &ty_void);
}

static void at_file(Sema *s, Node *d) { s->filename = d->file ? d->file : s->main_file; }

static void class_prepare(Sema *s, Node *c) {
    if (c->class_decl.state == 2) return;
    const char *name = c->class_decl.name;
    if (c->class_decl.state == 1) {
        report_error(s->filename, c->line, c->col,
                     "class '%s' inherits from itself", name);
        return;
    }
    c->class_decl.state = 1;
    const char *saved_fn = s->filename;
    at_file(s, c);

    Node *par = NULL;
    if (c->class_decl.parent) {
        Symbol *y = lookup_type(s, c->class_decl.parent);
        if (!y || y->kind != SYM_CLASS) {
            report_error(s->filename, c->line, c->col,
                         "the parent of class '%s', '%s', is not a class",
                         name, c->class_decl.parent);
        } else {
            par = y->decl;
            class_prepare(s, par);
            if (par->class_decl.state != 2) par = NULL;      /* cycle */
        }
    }
    c->class_decl.parent_decl = par;

    int nm = 0;
    for (Method *m = c->class_decl.methods; m; m = m->next) nm++;
    int n = par ? par->class_decl.nvslots : 0;
    Method **slots = arena_alloc(s->arena, (size_t)(n + nm + 1) * sizeof *slots);
    for (int i = 0; i < n; i++) slots[i] = par->class_decl.vslots[i];

    for (Field *f = c->class_decl.fields; f; f = f->next) {
        resolve_type(s, f->type, f->line, f->col);
        if (f->type->kind == TY_VOID)
            report_error(s->filename, f->line, f->col,
                         "field '%s' cannot have type void", f->name);
        if (f->type->kind == TY_ARRAY && f->type->array_size <= 0)
            report_error(s->filename, f->line, f->col,
                         "array field '%s' needs a size, e.g. int[4]", f->name);
        for (Field *g = c->class_decl.fields; g != f; g = g->next)
            if (!strcmp(g->name, f->name))
                report_error(s->filename, f->line, f->col,
                             "duplicate field '%s' in class '%s'", f->name, name);
        if (par && find_cfield(par, f->name, NULL, NULL))
            report_error(s->filename, f->line, f->col,
                         "field '%s' is already declared in a parent of '%s'",
                         f->name, name);
    }

    for (Method *m = c->class_decl.methods; m; m = m->next) {
        m->owner = name;
        m->slot = -1;
        if (m->is_abstract && !c->class_decl.is_abstract)
            report_error(s->filename, m->line, m->col,
                         "abstract method '%s' needs the class '%s' to be declared "
                         "'moolam vagupu'", m->name, name);
        TypeInfo *rt = m->ret_type ? m->ret_type : &ty_void;
        func_header(s, rt, m->params, m->line, m->col);
        for (Param *p = m->params, *q; p; p = p->next)
            for (q = m->params; q != p; q = q->next)
                if (!strcmp(p->name, q->name))
                    report_error(s->filename, m->line, m->col,
                                 "duplicate parameter '%s'", p->name);
        if (m->is_ctor) {
            if (c->class_decl.ctor)
                report_error(s->filename, m->line, m->col,
                             "class '%s' already has a constructor (overloading "
                             "is not supported)", name);
            else c->class_decl.ctor = m;
            continue;
        }
        for (Method *g = c->class_decl.methods; g != m; g = g->next)
            if (!g->is_ctor && !strcmp(g->name, m->name))
                report_error(s->filename, m->line, m->col,
                             "duplicate method '%s' in class '%s' (overloading "
                             "is not supported)", m->name, name);
        if (m->is_static) continue;
        Method *pm = par ? find_method(par, m->name, NULL) : NULL;
        if (pm) {
            if (pm->is_static)
                report_error(s->filename, m->line, m->col,
                             "method '%s' cannot override a static method", m->name);
            else if (!same_sig(m, pm))
                report_error(s->filename, m->line, m->col,
                             "method '%s' overrides '%s.%s' with a different "
                             "signature", m->name, pm->owner, pm->name);
            else {
                if (pm->access != ACC_PRIVATE) m->slot = pm->slot;
                m->overrides = pm;
            }
            if (m->slot < 0 && pm->slot >= 0 && pm->access != ACC_PRIVATE)
                m->slot = pm->slot;
        }
        if (m->slot >= 0) slots[m->slot] = m;
        else { m->slot = n; slots[n++] = m; }
    }
    c->class_decl.vslots  = slots;
    c->class_decl.nvslots = n;
    c->class_decl.state   = 2;
    if (!c->class_decl.is_abstract)
        for (int i = 0; i < n; i++)
            if (slots[i]->is_abstract && slots[i]->owner &&
                strcmp(slots[i]->owner, name) != 0)
                report_error(s->filename, c->line, c->col,
                             "class '%s' must implement the abstract method '%s' "
                             "of '%s'", name, slots[i]->name, slots[i]->owner);
    s->filename = saved_fn;
}

static void class_body(Sema *s, Node *c) {
    Node *saved_c = s->cur_class;
    Method *saved_m = s->cur_method;
    s->cur_class = c;
    const char *name = c->class_decl.name;
    Node *par = c->class_decl.parent_decl;
    Method *pc = par ? eff_ctor(par) : NULL;

    if (par && pc && pc->params && !c->class_decl.ctor)
        report_error(s->filename, c->line, c->col,
                     "class '%s' needs a constructor: the constructor of '%s' "
                     "takes arguments, so call it with thandhai(...)", name,
                     par->class_decl.name);

    for (Field *f = c->class_decl.fields; f; f = f->next) {
        if (!f->init) continue;
        s->cur_method = NULL;
        TypeInfo *it = expr(s, f->init);
        if (!assignable(s, f->type, it, f->init))
            report_error(s->filename, f->line, f->col,
                         "cannot initialise field '%s' (%s) with a value of "
                         "type %s%s", f->name, type_str(s, f->type),
                         type_str(s, it), conv_hint(s, f->type, it));
    }

    for (Method *m = c->class_decl.methods; m; m = m->next) {
        s->cur_method = m;
        s->ctor_call = NULL;
        if (m->is_abstract) continue;
        if (m->is_ctor) {
            NodeList *first = m->body->block.stmts;
            if (first && first->node->kind == NK_EXPR_STMT &&
                first->node->expr_stmt.expr->kind == NK_CTOR_CALL)
                s->ctor_call = first->node->expr_stmt.expr;
            else if (par && pc && pc->params)
                report_error(s->filename, m->line, m->col,
                             "the constructor of '%s' must start with "
                             "thandhai(...) because the parent constructor "
                             "takes arguments", name);
        }
        body_with_params(s, m->params, m->is_ctor ? &ty_void
                         : (m->ret_type ? m->ret_type : &ty_void),
                         m->body, m->line, m->col);
        if (!m->is_ctor)
            check_returns(s, m->ret_type, m->body, m->line, m->col,
                          tfmt(s, "method '%s'", m->name));
    }
    s->ctor_call = NULL;
    s->cur_class = saved_c;
    s->cur_method = saved_m;
}

/* ─── Statements ───────────────────────────────────────────────────────── */
static bool is_pure(Node *n) {
    switch (n->kind) {
        case NK_IDENT: case NK_THIS: case NK_INT_LIT: return true;
        case NK_MEMBER: return n->member.is_static || is_pure(n->member.object);
        case NK_INDEX:  return is_pure(n->index.array) && is_pure(n->index.index);
        default: return false;
    }
}

static void switch_stmt(Sema *s, Node *n) {
    TypeInfo *st = expr(s, n->switch_stmt.subject);
    if (!is_integral(st))
        report_error(s->filename, n->switch_stmt.subject->line,
                     n->switch_stmt.subject->col,
                     "'thervu' needs an integer, char or enum value, not %s "
                     "(use an if chain for strings and floats)", type_str(s, st));
    int cap = 0;
    for (SwitchCase *c = n->switch_stmt.cases; c; c = c->next)
        for (NodeList *l = c->labels; l; l = l->next) cap++;
    long long *seen = arena_alloc(s->arena, (size_t)(cap + 1) * sizeof *seen);
    int ns = 0;

    s->loop_depth++;
    for (SwitchCase *c = n->switch_stmt.cases; c; c = c->next) {
        for (NodeList *l = c->labels; l; l = l->next) {
            Node *lab = l->node;
            TypeInfo *lt = expr(s, lab);
            long long v;
            if (!is_integral(lt) || !const_eval(lab, &v)) {
                report_error(s->filename, lab->line, lab->col,
                             "a 'vazhakku' label must be a constant integer, "
                             "char or enum value");
                continue;
            }
            if (!is_unknown(st) && st->kind == TY_INT && !value_fits(st, v, false))
                report_error(s->filename, lab->line, lab->col,
                             "label %lld can never match a %s", v, type_str(s, st));
            for (int i = 0; i < ns; i++)
                if (seen[i] == v)
                    report_error(s->filename, lab->line, lab->col,
                                 "duplicate 'vazhakku' label %lld", v);
            seen[ns++] = v;
            lab->kind = NK_INT_LIT;               /* codegen prints the value */
            lab->int_lit.value = v;
            lab->int_lit.is_char = false;
            lab->int_lit.is_unsigned = false;
        }
        stmt(s, c->body);
    }
    s->loop_depth--;
}

static void hoist_program(Sema *s, Node *n) {
    /* A: names of types */
    for (NodeList *it = n->program.decls; it; it = it->next) {
        Node *d = it->node;
        at_file(s, d);
        Symbol *y = NULL;
        switch (d->kind) {
            case NK_STRUCT_DECL: {
                TypeInfo *t = type_new(s->arena, TY_STRUCT);
                t->struct_name = d->struct_decl.name;
                y = declare(s, d->struct_decl.name, SYM_STRUCT, t, d->line, d->col);
                if (y) y->fields = d->struct_decl.fields;
                break;
            }
            case NK_ENUM_DECL: {
                TypeInfo *t = type_new(s->arena, TY_ENUM);
                t->struct_name = d->enum_decl.name;
                y = declare(s, d->enum_decl.name, SYM_ENUM, t, d->line, d->col);
                if (y) y->enums = d->enum_decl.items;
                break;
            }
            case NK_CLASS_DECL: {
                TypeInfo *t = type_new(s->arena, TY_CLASS);
                t->struct_name = d->class_decl.name;
                y = declare(s, d->class_decl.name, SYM_CLASS, t, d->line, d->col);
                break;
            }
            case NK_TYPEDEF_DECL:
                y = declare(s, d->typedef_decl.name, SYM_TYPEDEF,
                            d->typedef_decl.target, d->line, d->col);
                break;
            default: break;
        }
        if (y) y->decl = d;
    }
    /* B: shapes of types */
    for (NodeList *it = n->program.decls; it; it = it->next) {
        Node *d = it->node;
        at_file(s, d);
        switch (d->kind) {
            case NK_STRUCT_DECL: struct_check(s, d); break;
            case NK_ENUM_DECL: {
                Symbol *y = lookup_in(s->global, d->enum_decl.name);
                if (y && y->decl == d) enum_resolve(s, y);
                break;
            }
            case NK_TYPEDEF_DECL: {
                Symbol *y = lookup_in(s->global, d->typedef_decl.name);
                if (y && y->decl == d && y->state == 0) {
                    y->state = 1;
                    resolve_type(s, y->type, d->line, d->col);
                    y->state = 2;
                }
                break;
            }
            case NK_CLASS_DECL: class_prepare(s, d); break;
            default: break;
        }
    }
    /* C: functions */
    for (NodeList *it = n->program.decls; it; it = it->next) {
        Node *d = it->node;
        at_file(s, d);
        if (d->kind == NK_FUNC_DECL) {
            TypeInfo *rt = d->func_decl.ret_type ? d->func_decl.ret_type : &ty_void;
            func_header(s, rt, d->func_decl.params, d->line, d->col);
            Symbol *y = declare(s, d->func_decl.name, SYM_FUNC, rt, d->line, d->col);
            if (y) y->params = d->func_decl.params;
            if (!strcmp(d->func_decl.name, "main")) {
                s->has_main = true;
                if (d->file && s->main_file && strcmp(d->file, s->main_file))
                    report_error(s->filename, d->line, d->col,
                                 "'main' cannot be defined in a header file");
                if (rt->kind != TY_INT)
                    report_error(s->filename, d->line, d->col,
                                 "'main' must be declared to return int");
                if (d->func_decl.params)
                    report_error(s->filename, d->line, d->col,
                                 "'main' must not take parameters");
                if (d->func_decl.is_static)
                    report_error(s->filename, d->line, d->col,
                                 "'main' cannot be thangu (static)");
            }
        } else if (d->kind == NK_EXTERN_FUNC) {
            /* Report clashes against the binding file, not main. */
            const char *saved = s->filename;
            s->filename = d->extern_func.file;
            TypeInfo *rt = d->extern_func.ret_type ? d->extern_func.ret_type
                                                   : &ty_void;
            func_header(s, rt, d->extern_func.params, d->line, d->col);
            Symbol *y = declare(s, d->extern_func.name, SYM_FUNC, rt,
                                d->line, d->col);
            if (y) {
                y->params = d->extern_func.params;
                y->cname  = d->extern_func.cname;
            }
            s->filename = saved;
        }
    }
}

static void stmt(Sema *s, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case NK_PROGRAM:
            hoist_program(s, n);
            for (NodeList *it = n->program.decls; it; it = it->next) {
                at_file(s, it->node);
                stmt(s, it->node);
            }
            s->filename = s->main_file;
            break;

        case NK_FUNC_DECL:   func_decl(s, n);   break;
        case NK_VAR_DECL:    var_decl(s, n);    break;
        case NK_CLASS_DECL:  class_body(s, n);  break;
        case NK_STRUCT_DECL: case NK_ENUM_DECL: case NK_TYPEDEF_DECL:
        case NK_EXTERN_FUNC: case NK_EXTERN_HEADER:
            break;                              /* handled when hoisting */

        case NK_BLOCK:
            scope_push(s);
            {
                bool warned = false;
                for (NodeList *it = n->block.stmts; it; it = it->next) {
                    stmt(s, it->node);
                    NodeKind k = it->node->kind;
                    if (!warned && it->next &&
                        (k == NK_RETURN || k == NK_BREAK || k == NK_CONTINUE || k == NK_THROW)) {
                        warned = true;           /* one warning per block is enough */
                        report_warning(s->filename, it->next->node->line,
                                       it->next->node->col, "unreachable code");
                    }
                }
            }
            scope_pop(s);
            break;

        case NK_IF:
            for (IfBranch *b = n->if_stmt.branches; b; b = b->next) {
                if (b->cond) cond_check(s, b->cond, "an if condition");
                stmt(s, b->body);
            }
            break;

        case NK_WHILE:
        case NK_DO_WHILE:
            cond_check(s, n->while_stmt.cond, "a loop condition");
            s->loop_depth++;
            stmt(s, n->while_stmt.body);
            s->loop_depth--;
            break;

        case NK_FOR:
            scope_push(s);
            if (n->for_stmt.init) stmt(s, n->for_stmt.init);
            if (n->for_stmt.cond) cond_check(s, n->for_stmt.cond, "a loop condition");
            if (n->for_stmt.update) expr(s, n->for_stmt.update);
            s->loop_depth++;
            stmt(s, n->for_stmt.body);
            s->loop_depth--;
            scope_pop(s);
            break;

        case NK_SWITCH: switch_stmt(s, n); break;

        case NK_FOREACH: {
            TypeInfo *ct = expr(s, n->foreach_stmt.coll);
            TypeInfo *et = &ty_unknown;
            if (!is_unknown(ct)) {
                if (ct->kind == TY_ARRAY) {
                    et = ct->elem_type;
                    if (ct->array_size <= 0)
                        report_error(s->filename, n->line, n->col,
                                     "cannot loop over an array of unknown length");
                    if (!is_pure(n->foreach_stmt.coll))
                        report_error(s->filename, n->line, n->col,
                                     "loop over a variable, not a computed expression");
                } else if (ct->kind == TY_LIST) et = ct->elem_type;
                else if (ct->kind == TY_MAP) et = ct->key_type;      /* keys, in insertion order */
                else if (ct->kind == TY_STRING) et = &ty_char;
                else
                    report_error(s->filename, n->line, n->col,
                                 "'ulle' needs an array, a List or a string, not %s",
                                 type_str(s, ct));
            }
            TypeInfo *vt = n->foreach_stmt.vtype;
            if (vt) {
                resolve_type(s, vt, n->line, n->col);
                if (!assignable(s, vt, et, NULL))
                    report_error(s->filename, n->line, n->col,
                                 "loop variable '%s' (%s) cannot hold %s elements",
                                 n->foreach_stmt.name, type_str(s, vt), type_str(s, et));
                et = vt;
            }
            n->foreach_stmt.elem = et;
            scope_push(s);
            declare(s, n->foreach_stmt.name, SYM_VAR, et, n->line, n->col);
            s->loop_depth++;
            stmt(s, n->foreach_stmt.body);
            s->loop_depth--;
            scope_pop(s);
            break;
        }

        case NK_RETURN: {
            TypeInfo *want = s->current_ret ? s->current_ret : &ty_void;
            if (n->ret.value) {
                TypeInfo *t = expr(s, n->ret.value);
                if (want->kind == TY_VOID)
                    report_error(s->filename, n->line, n->col,
                                 "a void function cannot return a value");
                else if (!assignable(s, want, t, n->ret.value))
                    report_error(s->filename, n->line, n->col,
                                 "return type mismatch: expected %s but got %s%s",
                                 type_str(s, want), type_str(s, t),
                                 conv_hint(s, want, t));
            } else if (want->kind != TY_VOID) {
                report_error(s->filename, n->line, n->col,
                             "'kudu' needs a value of type %s",
                             type_str(s, want));
            }
            break;
        }

        case NK_TRY:
            stmt(s, n->try_stmt.body);
            scope_push(s);
            if (n->try_stmt.catch_name)
                declare(s, n->try_stmt.catch_name, SYM_VAR, &ty_string, n->line, n->col);
            stmt(s, n->try_stmt.catch_body);
            scope_pop(s);
            break;

        case NK_THROW: {
            TypeInfo *t = expr(s, n->throw_stmt.value);
            if (!is_unknown(t) && t->kind != TY_STRING)
                report_error(s->filename, n->line, n->col,
                             "'erivi' throws a string message, not %s",
                             type_str(s, t));
            break;
        }

        case NK_BREAK:
        case NK_CONTINUE:
            break;

        case NK_EXPR_STMT: expr(s, n->expr_stmt.expr); break;
        case NK_PATHI:     check_pathi(s, n);          break;

        default: expr(s, n); break;
    }
}

/* ─── Public API ───────────────────────────────────────────────────────── */
void sema_init(Sema *s, Arena *arena, const char *filename) {
    s->arena = arena;
    s->filename = filename;
    s->main_file = filename;
    s->current_ret = NULL;
    s->has_main = false;
    s->cur_class = NULL;
    s->cur_method = NULL;
    s->ctor_call = NULL;
    s->loop_depth = 0;
    s->scope = NULL;
    scope_push(s);
    s->global = s->scope;
}

void sema_analyse(Sema *s, Node *program) { stmt(s, program); }
