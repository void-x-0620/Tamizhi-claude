/*
 * codegen.c — AST -> C99/GNU C (see codegen.h for the optimisation list).
 *
 * Object model:
 *   • a class is a C struct; a derived class embeds its parent as the first
 *     member (_b), the root class owns the hidden vtable pointer (_vt)
 *   • every class has a vtable struct listing ALL its virtual methods
 *     (inherited ones first, so a slot has the same index in every subclass)
 *   • puthu C(args) calls C__new, which allocates a zeroed object, installs
 *     the vtable and runs the constructor chain
 *   • obj.m(args) is a vtable call; thandhai.m(args) and static methods are
 *     direct calls
 */
#include "codegen.h"
#include <limits.h>

static void gen_stmt(CodeGen *cg, Node *n);
static void gen_expr(CodeGen *cg, Node *n);
static void gen_block(CodeGen *cg, Node *blk);

/* ─── Output helpers ──────────────────────────────────────────────────── */
static void emit(CodeGen *cg, const char *s) { strbuf_append(&cg->out, s); }
static void emit_n(CodeGen *cg, const char *s, size_t n) {
    strbuf_append_n(&cg->out, s, n);
}
#define emitf(cg, ...) strbuf_appendf(&(cg)->out, __VA_ARGS__)

static void indent(CodeGen *cg) {
    for (int i = 0; i < cg->indent; i++) emit(cg, "    ");
}

/* printf into the arena. */
static const char *tfmt(CodeGen *cg, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static const char *tfmt(CodeGen *cg, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = arena_alloc(cg->arena, (size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

/* All user identifiers get a tz_ prefix, except the entry point. */
static void emit_ident(CodeGen *cg, const char *name) {
    if (!strcmp(name, "main")) emit(cg, "main");
    else emitf(cg, "tz_%s", name);
}

static const char *ident_str(CodeGen *cg, const char *name) {
    return !strcmp(name, "main") ? "main" : tfmt(cg, "tz_%s", name);
}

/* Emit a C string literal. Bytes >= 0x80 pass through so UTF-8 (e.g. Tamil
 * text) survives; control characters become 3-digit octal escapes. */
static void emit_c_string(CodeGen *cg, const char *s) {
    emit(cg, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '\n': emit(cg, "\\n");  break;
            case '\t': emit(cg, "\\t");  break;
            case '\r': emit(cg, "\\r");  break;
            case '\\': emit(cg, "\\\\"); break;
            case '"':  emit(cg, "\\\""); break;
            case '?':  emit(cg, "\\?");  break;     /* avoid trigraphs */
            default:
                if (*p < 0x20 || *p == 0x7f) emitf(cg, "\\%03o", *p);
                else emit_n(cg, (const char *)p, 1);
        }
    }
    emit(cg, "\"");
}

/* ─── Program lookups ─────────────────────────────────────────────────── */
static Node *find_decl(CodeGen *cg, NodeKind kind, const char *name) {
    for (NodeList *it = cg->prog->program.decls; it; it = it->next) {
        Node *d = it->node;
        if (d->kind != kind) continue;
        const char *n = kind == NK_CLASS_DECL ? d->class_decl.name
                                              : d->struct_decl.name;
        if (!strcmp(n, name)) return d;
    }
    return NULL;
}

static Node *class_of(CodeGen *cg, TypeInfo *t) {
    return t && t->kind == TY_CLASS ? find_decl(cg, NK_CLASS_DECL, t->struct_name)
                                    : NULL;
}

static Method *eff_ctor(Node *cls) {
    for (Node *c = cls; c; c = c->class_decl.parent_decl)
        if (c->class_decl.ctor) return c->class_decl.ctor;
    return NULL;
}

static Node *root_class(Node *c) {
    while (c->class_decl.parent_decl) c = c->class_decl.parent_decl;
    return c;
}

static int class_depth(Node *c) {
    int d = 0;
    while (c->class_decl.parent_decl) { c = c->class_decl.parent_decl; d++; }
    return d;
}

/* ─── Constant folding and peepholes ──────────────────────────────────── */
static bool is_int_class(Node *n) {
    return n->kind == NK_INT_LIT || n->kind == NK_BOOL_LIT;
}
static bool is_num_lit(Node *n) {
    return is_int_class(n) || n->kind == NK_FLOAT_LIT;
}
static bool is_unsigned_lit(Node *n) {
    return n->kind == NK_INT_LIT && n->int_lit.is_unsigned;
}
static long long ival(Node *n) {
    return n->kind == NK_BOOL_LIT ? (n->bool_lit.value ? 1 : 0)
                                  : n->int_lit.value;
}
static double fval(Node *n) {
    return n->kind == NK_FLOAT_LIT ? n->float_lit.value : (double)ival(n);
}

/* True if evaluating n cannot call a function or assign. */
static bool is_pure(Node *n) {
    switch (n->kind) {
        case NK_INT_LIT: case NK_FLOAT_LIT: case NK_STRING_LIT:
        case NK_BOOL_LIT: case NK_IDENT: case NK_NULL_LIT: case NK_THIS:
        case NK_SIZEOF:
            return true;
        case NK_BINARY: return is_pure(n->binary.left) && is_pure(n->binary.right);
        case NK_UNARY:  return is_pure(n->unary.operand);
        case NK_INDEX:  return is_pure(n->index.array) && is_pure(n->index.index);
        case NK_MEMBER: return n->member.is_static || is_pure(n->member.object);
        case NK_CAST:   return is_pure(n->cast.expr);
        default:        return false;
    }
}

static Node *mk_int(CodeGen *cg, Node *at, long long v) {
    Node *n = node_new(cg->arena, NK_INT_LIT, at->line, at->col);
    n->int_lit.value = v;
    n->type = &ty_int;
    return n;
}
static Node *mk_bool(CodeGen *cg, Node *at, bool v) {
    Node *n = node_new(cg->arena, NK_BOOL_LIT, at->line, at->col);
    n->bool_lit.value = v;
    n->type = &ty_bool;
    return n;
}
static Node *mk_float(CodeGen *cg, Node *at, double v) {
    Node *n = node_new(cg->arena, NK_FLOAT_LIT, at->line, at->col);
    n->float_lit.value = v;
    n->type = &ty_float;
    return n;
}

static Node *fold_binary(CodeGen *cg, Node *n) {
    Node *L = n->binary.left, *R = n->binary.right;
    BinOp op = n->binary.op;

    /* Short-circuit: the right side is never evaluated, so it may be impure. */
    if (op == BOP_AND && is_num_lit(L) && fval(L) == 0.0) return mk_bool(cg, n, false);
    if (op == BOP_OR  && is_num_lit(L) && fval(L) != 0.0) return mk_bool(cg, n, true);

    if (!is_num_lit(L) || !is_num_lit(R)) return NULL;
    if (is_unsigned_lit(L) || is_unsigned_lit(R)) return NULL;

    if (is_int_class(L) && is_int_class(R)) {
        long long a = ival(L), b = ival(R), r;
        switch (op) {
            case BOP_ADD: if (__builtin_add_overflow(a, b, &r)) return NULL;
                          return mk_int(cg, n, r);
            case BOP_SUB: if (__builtin_sub_overflow(a, b, &r)) return NULL;
                          return mk_int(cg, n, r);
            case BOP_MUL: if (__builtin_mul_overflow(a, b, &r)) return NULL;
                          return mk_int(cg, n, r);
            case BOP_DIV: if (b == 0 || (a == LLONG_MIN && b == -1)) return NULL;
                          return mk_int(cg, n, a / b);
            case BOP_MOD: if (b == 0 || (a == LLONG_MIN && b == -1)) return NULL;
                          return mk_int(cg, n, a % b);
            case BOP_EQ:  return mk_bool(cg, n, a == b);
            case BOP_NE:  return mk_bool(cg, n, a != b);
            case BOP_LT:  return mk_bool(cg, n, a <  b);
            case BOP_LE:  return mk_bool(cg, n, a <= b);
            case BOP_GT:  return mk_bool(cg, n, a >  b);
            case BOP_GE:  return mk_bool(cg, n, a >= b);
            case BOP_AND: return mk_bool(cg, n, a && b);
            case BOP_OR:  return mk_bool(cg, n, a || b);
            case BOP_BAND: return mk_int(cg, n, a & b);
            case BOP_BOR:  return mk_int(cg, n, a | b);
            case BOP_BXOR: return mk_int(cg, n, a ^ b);
            case BOP_SHL: if (a < 0 || b < 0 || b > 62) return NULL;
                          r = (long long)((unsigned long long)a << b);
                          if ((r >> b) != a) return NULL;
                          return mk_int(cg, n, r);
            case BOP_SHR: if (a < 0 || b < 0 || b > 63) return NULL;
                          return mk_int(cg, n, a >> b);
        }
        return NULL;
    }

    double a = fval(L), b = fval(R);
    switch (op) {
        case BOP_ADD: return mk_float(cg, n, a + b);
        case BOP_SUB: return mk_float(cg, n, a - b);
        case BOP_MUL: return mk_float(cg, n, a * b);
        case BOP_DIV: return b == 0.0 ? NULL : mk_float(cg, n, a / b);
        case BOP_EQ:  return mk_bool(cg, n, a == b);
        case BOP_NE:  return mk_bool(cg, n, a != b);
        case BOP_LT:  return mk_bool(cg, n, a <  b);
        case BOP_LE:  return mk_bool(cg, n, a <= b);
        case BOP_GT:  return mk_bool(cg, n, a >  b);
        case BOP_GE:  return mk_bool(cg, n, a >= b);
        case BOP_AND: return mk_bool(cg, n, a != 0.0 && b != 0.0);
        case BOP_OR:  return mk_bool(cg, n, a != 0.0 || b != 0.0);
        default:      return NULL;
    }
}

static bool lit_is_int(Node *n, long long v) {
    return n->kind == NK_INT_LIT && n->int_lit.value == v && !n->int_lit.is_unsigned;
}

static Node *peephole(Node *n) {
    Node *L = n->binary.left, *R = n->binary.right;
    /* Only rewrite when the surviving operand already has the result type
     * (so e.g. u8 + 0 keeps its C promotion). */
    bool same = L->type && R->type && n->type && L->type == n->type;
    bool sameR = L->type && R->type && n->type && R->type == n->type;
    switch (n->binary.op) {
        case BOP_ADD:
            if (lit_is_int(R, 0) && same) return L;
            if (lit_is_int(L, 0) && sameR) return R;
            break;
        case BOP_SUB:
            if (lit_is_int(R, 0) && same) return L;
            break;
        case BOP_MUL:
            if (lit_is_int(R, 1) && same) return L;
            if (lit_is_int(L, 1) && sameR) return R;
            /* x*0 is only safe for pure integer x (no side effects, and
             * float NaN/inf would not give 0). */
            if (lit_is_int(R, 0) && is_pure(L) && L->type &&
                L->type->kind == TY_INT && n->type == &ty_int && L->type == &ty_int)
                return R;
            if (lit_is_int(L, 0) && is_pure(R) && R->type &&
                R->type->kind == TY_INT && n->type == &ty_int && R->type == &ty_int)
                return L;
            break;
        case BOP_DIV:
            if (lit_is_int(R, 1) && same) return L;
            break;
        default: break;
    }
    return n;
}

static void simplify_list(CodeGen *cg, NodeList *l);

/* Simplify bottom-up and return the (possibly replaced) node. */
static Node *simplify(CodeGen *cg, Node *n) {
    switch (n->kind) {
        case NK_BINARY: {
            n->binary.left  = simplify(cg, n->binary.left);
            n->binary.right = simplify(cg, n->binary.right);
            Node *f = fold_binary(cg, n);
            return f ? f : peephole(n);
        }
        case NK_UNARY: {
            if (n->unary.op == UOP_ADDR || n->unary.op == UOP_DEREF) {
                n->unary.operand = simplify(cg, n->unary.operand);
                return n;
            }
            Node *o = n->unary.operand = simplify(cg, n->unary.operand);
            if (n->unary.op == UOP_NOT) {
                if (is_num_lit(o)) return mk_bool(cg, n, fval(o) == 0.0);
                /* !!x == x only when x is already a bool */
                if (o->kind == NK_UNARY && o->unary.op == UOP_NOT &&
                    o->unary.operand->type &&
                    o->unary.operand->type->kind == TY_BOOL)
                    return o->unary.operand;
            } else if (n->unary.op == UOP_NEG) {
                if (o->kind == NK_INT_LIT && !o->int_lit.is_unsigned &&
                    o->int_lit.value != LLONG_MIN)
                    return mk_int(cg, n, -o->int_lit.value);
                if (o->kind == NK_FLOAT_LIT)
                    return mk_float(cg, n, -o->float_lit.value);
            } else if (n->unary.op == UOP_BNOT) {
                if (o->kind == NK_INT_LIT && !o->int_lit.is_unsigned)
                    return mk_int(cg, n, ~o->int_lit.value);
            }
            return n;
        }
        case NK_ASSIGN:
            n->assign.value = simplify(cg, n->assign.value);
            n->assign.target = simplify(cg, n->assign.target);
            return n;
        case NK_CALL:  simplify_list(cg, n->call.args); return n;
        case NK_NEW:   simplify_list(cg, n->new_expr.args); return n;
        case NK_LIST_NEW: simplify_list(cg, n->list_new.items); return n;
        case NK_CTOR_CALL: simplify_list(cg, n->ctor_call.args); return n;
        case NK_MCALL:
            if (n->mcall.object) n->mcall.object = simplify(cg, n->mcall.object);
            simplify_list(cg, n->mcall.args);
            return n;
        case NK_INDEX:
            n->index.array = simplify(cg, n->index.array);
            n->index.index = simplify(cg, n->index.index);
            return n;
        case NK_MEMBER:
            if (!n->member.is_static)
                n->member.object = simplify(cg, n->member.object);
            return n;
        case NK_CAST:
            n->cast.expr = simplify(cg, n->cast.expr);
            return n;
        case NK_INCDEC:
            n->incdec.operand = simplify(cg, n->incdec.operand);
            return n;
        case NK_STRUCT_LIT:
            for (FieldInit *fi = n->struct_lit.inits; fi; fi = fi->next)
                fi->value = simplify(cg, fi->value);
            return n;
        case NK_TERNARY: {
            n->ternary.cond   = simplify(cg, n->ternary.cond);
            n->ternary.then_e = simplify(cg, n->ternary.then_e);
            n->ternary.else_e = simplify(cg, n->ternary.else_e);
            if (is_num_lit(n->ternary.cond) && n->ternary.then_e->type == n->type &&
                n->ternary.else_e->type == n->type)
                return fval(n->ternary.cond) != 0.0 ? n->ternary.then_e
                                                    : n->ternary.else_e;
            return n;
        }
        default:
            return n;
    }
}

static void simplify_list(CodeGen *cg, NodeList *l) {
    for (; l; l = l->next) l->node = simplify(cg, l->node);
}

/* ─── C declarators ───────────────────────────────────────────────────── */
static const char *decl_str(CodeGen *cg, TypeInfo *t, const char *inner);

/* Every distinct List<T> becomes a C struct plus helper functions. The
 * definitions go into cg->lists and are placed after the type definitions. */
static int list_index(CodeGen *cg, TypeInfo *elem) {
    const char *key = decl_str(cg, elem, "");      /* registers nested lists first */
    for (int i = 0; i < cg->nlists; i++)
        if (!strcmp(cg->list_keys[i], key)) return i;
    if (cg->nlists >= 256) { fputs("tamizhi: too many List<T> types\n", stderr); exit(1); }
    int n = cg->nlists++;
    cg->list_keys[n] = key;
    cg->uses_heap = true;
    if (cg->checks) cg->uses_checks = true;

    const char *L  = tfmt(cg, "tz_List_%d", n);
    const char *V  = decl_str(cg, elem, "v");
    const char *PE = decl_str(cg, type_pointer(cg->arena, elem), "");
    StrBuf *o = &cg->lists;
    bool ck = cg->checks;
    const char *nn = ck ? "    if (!l) tz_fail(line, \"null list (illai) used\", 0, 0);\n" : "";

    strbuf_appendf(o, "struct %s { %s; long long len, cap; };\n", L,
                   decl_str(cg, elem, "*data") );
    strbuf_appendf(o, "static %s *%s_new(void) __attribute__((unused));\n"
                      "static %s *%s_new(void) { return (%s *)tz_alloc_obj(sizeof(%s)); }\n",
                   L, L, L, L, L, L);
    /* length */
    strbuf_appendf(o, "static long long %s_len(%s *l, int line) __attribute__((unused));\n"
                      "static long long %s_len(%s *l, int line) {\n%s    return l->len;\n}\n",
                   L, L, L, L, nn);
    /* push */
    strbuf_appendf(o, "static void %s_push(%s *l, %s, int line) __attribute__((unused));\n"
                      "static void %s_push(%s *l, %s, int line) {\n%s"
                      "    if (l->len == l->cap) {\n"
                      "        long long nc = l->cap ? l->cap * 2 : 4;\n"
                      "        l->data = (%s)tz_grow(l->data, (size_t)l->cap * sizeof(%s), (size_t)nc * sizeof(%s));\n"
                      "        l->cap = nc;\n    }\n"
                      "    l->data[l->len++] = v;\n}\n",
                   L, L, V, L, L, V, nn, PE, key, key);
    /* at (element address, bounds checked) */
    strbuf_appendf(o, "static %s;\n", decl_str(cg, type_pointer(cg->arena, elem),
                   tfmt(cg, "%s_at(%s *l, long long i, int line) __attribute__((unused))", L, L)));
    strbuf_appendf(o, "static %s {\n%s", decl_str(cg, type_pointer(cg->arena, elem),
                   tfmt(cg, "%s_at(%s *l, long long i, int line)", L, L)), nn);
    if (ck) strbuf_append(o, "    if (i < 0 || i >= l->len) tz_fail(line, \"list index %lld is out of range (length %lld)\", i, l->len);\n");
    strbuf_append(o, "    return &l->data[i];\n}\n");
    /* pop */
    strbuf_appendf(o, "static %s;\n", decl_str(cg, elem, tfmt(cg, "%s_pop(%s *l, int line) __attribute__((unused))", L, L)));
    strbuf_appendf(o, "static %s {\n%s", decl_str(cg, elem, tfmt(cg, "%s_pop(%s *l, int line)", L, L)), nn);
    if (ck) strbuf_append(o, "    if (l->len == 0) tz_fail(line, \"pop from an empty list\", 0, 0);\n");
    strbuf_append(o, "    return l->data[--l->len];\n}\n");
    /* insert */
    strbuf_appendf(o, "static void %s_insert(%s *l, long long i, %s, int line) __attribute__((unused));\n"
                      "static void %s_insert(%s *l, long long i, %s, int line) {\n%s",
                   L, L, V, L, L, V, nn);
    if (ck) strbuf_append(o, "    if (i < 0 || i > l->len) tz_fail(line, \"insert position %lld is out of range (length %lld)\", i, l->len);\n");
    strbuf_appendf(o, "    %s_push(l, v, line);\n"
                      "    memmove(&l->data[i + 1], &l->data[i], (size_t)(l->len - 1 - i) * sizeof(%s));\n"
                      "    l->data[i] = v;\n}\n", L, key);
    /* remove */
    strbuf_appendf(o, "static %s;\n", decl_str(cg, elem, tfmt(cg, "%s_remove(%s *l, long long i, int line) __attribute__((unused))", L, L)));
    strbuf_appendf(o, "static %s {\n%s", decl_str(cg, elem, tfmt(cg, "%s_remove(%s *l, long long i, int line)", L, L)), nn);
    if (ck) strbuf_append(o, "    if (i < 0 || i >= l->len) tz_fail(line, \"list index %lld is out of range (length %lld)\", i, l->len);\n");
    strbuf_appendf(o, "    %s = l->data[i];\n"
                      "    memmove(&l->data[i], &l->data[i + 1], (size_t)(l->len - i - 1) * sizeof(%s));\n"
                      "    l->len--;\n    return v;\n}\n", decl_str(cg, elem, "v"), key);
    /* clear */
    strbuf_appendf(o, "static void %s_clear(%s *l, int line) __attribute__((unused));\n"
                      "static void %s_clear(%s *l, int line) {\n%s    l->len = 0;\n}\n\n",
                   L, L, L, L, nn);
    return n;
}

/* Map<K,V> / Set<T>: an insertion-ordered open-addressing hash table. The
 * entries live in two parallel arrays (iteration order = insertion order);
 * `tab` maps hash slots to entry index + 1. */
static int map_index(CodeGen *cg, TypeInfo *mt) {
    const char *K = decl_str(cg, mt->key_type, "");
    const char *V = decl_str(cg, mt->elem_type, "");
    const char *key = tfmt(cg, "%s|%s|%d", K, V, mt->is_set);
    for (int i = 0; i < cg->nmaps; i++)
        if (!strcmp(cg->map_keys[i], key)) return i;
    if (cg->nmaps >= 256) { fputs("tamizhi: too many Map/Set types\n", stderr); exit(1); }
    int n = cg->nmaps++;
    cg->map_keys[n] = key;
    cg->uses_heap = true;
    if (cg->checks) cg->uses_checks = true;

    const char *M = tfmt(cg, "tz_Map_%d", n);
    const char *Kd = decl_str(cg, mt->key_type, "k");
    const char *Vp = decl_str(cg, type_pointer(cg->arena, mt->elem_type), "");
    bool str = mt->key_type->kind == TY_STRING;
    const char *hash = str
        ? "    unsigned long long h = 1469598103934665603ULL;\n"
          "    for (const unsigned char *p = (const unsigned char *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }\n"
          "    return h;\n"
        : "    unsigned long long h = (unsigned long long)k * 0x9E3779B97F4A7C15ULL;\n"
          "    return h ^ (h >> 29);\n";
    const char *eq = str ? "!strcmp(m->keys[e], k)" : "m->keys[e] == k";
    const char *nn = cg->checks
        ? "    if (!m) tz_fail(line, \"null Map/Set (illai) used\", 0, 0);\n" : "";
    const char *miss = cg->checks
        ? "    if (i < 0) tz_fail(line, \"key not found in the Map\", 0, 0);\n"
        : "    if (i < 0) { memset(&z, 0, sizeof z); return z; }\n";
    StrBuf *o = &cg->lists;

    strbuf_appendf(o, "struct %s { %s *keys; %s *vals; long long *tab; long long len, cap, tcap; };\n",
                   M, K, V);
    strbuf_appendf(o, "static %s *%s_new(void) __attribute__((unused));\n"
                      "static %s *%s_new(void) { return (%s *)tz_alloc_obj(sizeof(%s)); }\n",
                   M, M, M, M, M, M);
    strbuf_appendf(o, "static unsigned long long %s_hash(%s) {\n%s}\n", M, Kd, hash);
    strbuf_appendf(o, "static void %s_rebuild(%s *m) {\n"
                      "    long long tc = 8;\n    while (tc < m->cap * 2) tc *= 2;\n"
                      "    m->tab = (long long *)tz_alloc_obj((size_t)tc * sizeof(long long));\n    m->tcap = tc;\n"
                      "    for (long long i = 0; i < m->len; i++) {\n"
                      "        unsigned long long h = %s_hash(m->keys[i]) & (unsigned long long)(tc - 1);\n"
                      "        while (m->tab[h]) h = (h + 1) & (unsigned long long)(tc - 1);\n"
                      "        m->tab[h] = i + 1;\n    }\n}\n", M, M, M);
    strbuf_appendf(o, "static long long %s_find(%s *m, %s) {\n"
                      "    if (!m->tcap) return -1;\n"
                      "    unsigned long long h = %s_hash(k) & (unsigned long long)(m->tcap - 1);\n"
                      "    while (m->tab[h]) {\n"
                      "        long long e = m->tab[h] - 1;\n"
                      "        if (%s) return e;\n"
                      "        h = (h + 1) & (unsigned long long)(m->tcap - 1);\n    }\n    return -1;\n}\n",
                   M, M, Kd, M, eq);
    strbuf_appendf(o, "static long long %s_slot(%s *m, %s) {           /* index of k, adding it if new */\n"
                      "    long long i = %s_find(m, k);\n    if (i >= 0) return i;\n"
                      "    if (m->len == m->cap) {\n"
                      "        long long nc = m->cap ? m->cap * 2 : 4;\n"
                      "        m->keys = (%s *)tz_grow(m->keys, (size_t)m->cap * sizeof(%s), (size_t)nc * sizeof(%s));\n"
                      "        m->vals = (%s *)tz_grow(m->vals, (size_t)m->cap * sizeof(%s), (size_t)nc * sizeof(%s));\n"
                      "        m->cap = nc;\n        %s_rebuild(m);\n    }\n"
                      "    m->keys[m->len] = k;\n    memset(&m->vals[m->len], 0, sizeof(%s));\n    m->len++;\n"
                      "    unsigned long long h = %s_hash(k) & (unsigned long long)(m->tcap - 1);\n"
                      "    while (m->tab[h]) h = (h + 1) & (unsigned long long)(m->tcap - 1);\n"
                      "    m->tab[h] = m->len;\n    return m->len - 1;\n}\n",
                   M, M, Kd, M, K, K, K, V, V, V, M, V, M);
    strbuf_appendf(o, "static long long %s_len(%s *m, int line) __attribute__((unused));\n"
                      "static long long %s_len(%s *m, int line) {\n%s    return m->len;\n}\n", M, M, M, M, nn);
    strbuf_appendf(o, "static %s *%s_ref(%s *m, %s, int line) __attribute__((unused));\n"
                      "static %s *%s_ref(%s *m, %s, int line) {\n%s    long long i = %s_slot(m, k);\n    return &m->vals[i];\n}\n",
                   V, M, M, Kd, V, M, M, Kd, nn, M);
    strbuf_appendf(o, "static %s %s_get(%s *m, %s, int line) __attribute__((unused));\n"
                      "static %s %s_get(%s *m, %s, int line) {\n%s%s    long long i = %s_find(m, k);\n%s"
                      "    return m->vals[i];\n}\n",
                   V, M, M, Kd, V, M, M, Kd, nn,
                   cg->checks ? "" : tfmt(cg, "    %s z;\n", V), M, miss);
    strbuf_appendf(o, "static %s %s_get_or(%s *m, %s, %s, int line) __attribute__((unused));\n"
                      "static %s %s_get_or(%s *m, %s, %s, int line) {\n%s    long long i = %s_find(m, k);\n"
                      "    return i < 0 ? d : m->vals[i];\n}\n",
                   V, M, M, Kd, decl_str(cg, mt->elem_type, "d"),
                   V, M, M, Kd, decl_str(cg, mt->elem_type, "d"), nn, M);
    strbuf_appendf(o, "static int %s_has(%s *m, %s, int line) __attribute__((unused));\n"
                      "static int %s_has(%s *m, %s, int line) {\n%s    return %s_find(m, k) >= 0;\n}\n",
                   M, M, Kd, M, M, Kd, nn, M);
    strbuf_appendf(o, "static int %s_insert(%s *m, %s, int line) __attribute__((unused));\n"
                      "static int %s_insert(%s *m, %s, int line) {\n%s    long long before = m->len;\n"
                      "    %s_slot(m, k);\n    return m->len != before;\n}\n",
                   M, M, Kd, M, M, Kd, nn, M);
    strbuf_appendf(o, "static int %s_remove(%s *m, %s, int line) __attribute__((unused));\n"
                      "static int %s_remove(%s *m, %s, int line) {\n%s    long long i = %s_find(m, k);\n"
                      "    if (i < 0) return 0;\n"
                      "    memmove(&m->keys[i], &m->keys[i + 1], (size_t)(m->len - i - 1) * sizeof(%s));\n"
                      "    memmove(&m->vals[i], &m->vals[i + 1], (size_t)(m->len - i - 1) * sizeof(%s));\n"
                      "    m->len--;\n    %s_rebuild(m);\n    return 1;\n}\n",
                   M, M, Kd, M, M, Kd, nn, M, K, V, M);
    strbuf_appendf(o, "static void %s_clear(%s *m, int line) __attribute__((unused));\n"
                      "static void %s_clear(%s *m, int line) {\n%s    m->len = 0;\n"
                      "    if (m->tcap) memset(m->tab, 0, (size_t)m->tcap * sizeof(long long));\n}\n\n",
                   M, M, M, M, nn);
    (void)Vp;
    return n;
}

static const char *decl_str(CodeGen *cg, TypeInfo *t, const char *inner) {
    if (!t) t = &ty_unknown;
    switch (t->kind) {
        case TY_POINTER:
            return decl_str(cg, t->elem_type, tfmt(cg, "*%s", inner));
        case TY_ARRAY: {
            const char *in = inner[0] == '*' ? tfmt(cg, "(%s)", inner) : inner;
            return decl_str(cg, t->elem_type,
                            t->array_size > 0 ? tfmt(cg, "%s[%d]", in, t->array_size)
                                              : tfmt(cg, "%s[]", in));
        }
        case TY_FUNC: {
            const char *ps = "";
            for (Param *p = t->fparams; p; p = p->next)
                ps = tfmt(cg, "%s%s%s", ps, decl_str(cg, p->type, ""),
                          p->next ? ", " : "");
            if (!t->fparams) ps = "void";
            return decl_str(cg, t->fret, tfmt(cg, "(*%s)(%s)", inner, ps));
        }
        default: break;
    }
    const char *base;
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: base = t->cname; break;
        case TY_STRING: base = "const char *"; break;
        case TY_BOOL:   base = "int"; break;
        case TY_VOID:   base = "void"; break;
        case TY_NULL:   base = "void *"; break;
        case TY_ENUM:   base = tfmt(cg, "tz_%s", t->struct_name); break;
        case TY_STRUCT: base = tfmt(cg, "tz_%s", t->struct_name); break;
        case TY_CLASS:  base = tfmt(cg, "tz_%s *", t->struct_name); break;
        case TY_LIST:   base = tfmt(cg, "tz_List_%d *", list_index(cg, t->elem_type)); break;
        case TY_MAP:    base = tfmt(cg, "tz_Map_%d *", map_index(cg, t)); break;
        default:        base = "long long"; break;
    }
    size_t bl = strlen(base);
    bool nosep = !inner[0] || base[bl - 1] == '*';
    return tfmt(cg, "%s%s%s", base, nosep ? "" : " ", inner);
}

static const char *ptype_dummy(CodeGen *cg, Param *p, const char *name) {
    TypeInfo *t = p->type ? p->type : &ty_unknown;
    if (t->kind == TY_ARRAY)                    /* arrays decay when passed */
        t = type_pointer(cg->arena, t->elem_type);
    return decl_str(cg, t, name);
}

static const char *params_str(CodeGen *cg, Param *params, bool named,
                              const char *first) {
    const char *r = first ? first : "";
    for (Param *p = params; p; p = p->next) {
        const char *d = ptype_dummy(cg, p, named ? ident_str(cg, p->name) : "");
        r = tfmt(cg, "%s%s%s", r, d, p->next ? ", " : "");
    }
    if (!r[0]) r = "void";
    return r;
}

/* A variable declaration with qualifiers. */
static const char *var_decl_str(CodeGen *cg, TypeInfo *t, const char *name,
                                int quals, bool keep_const) {
    if (!keep_const) quals &= ~Q_CONST;
    bool ptrish = t && (t->kind == TY_POINTER || t->kind == TY_FUNC ||
                        t->kind == TY_CLASS || t->kind == TY_STRING);
    const char *inner = name;
    const char *prefix = "";
    if (quals & Q_STATIC) prefix = "static ";
    if (ptrish) {
        if (quals & Q_VOLATILE) inner = tfmt(cg, "volatile %s", inner);
        if (quals & Q_CONST)    inner = tfmt(cg, "const %s", inner);
    } else {
        if (quals & Q_VOLATILE) prefix = tfmt(cg, "%svolatile ", prefix);
        if (quals & Q_CONST)    prefix = tfmt(cg, "%sconst ", prefix);
    }
    return tfmt(cg, "%s%s", prefix, decl_str(cg, t, inner));
}

static void gen_zero_init(CodeGen *cg, TypeInfo *t) {
    switch (t ? t->kind : TY_UNKNOWN) {
        case TY_FLOAT:  emit(cg, "0.0"); break;
        case TY_STRING: emit(cg, "\"\""); break;
        case TY_ARRAY:
        case TY_STRUCT: emit(cg, "{0}"); break;
        default:        emit(cg, "0");   break;
    }
}

/* ─── Expressions ─────────────────────────────────────────────────────── */
static void gen_float(CodeGen *cg, double v, TypeInfo *t) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    emit(cg, buf);
    if (!strpbrk(buf, ".eEnN")) emit(cg, ".0");   /* keep it a double */
    if (t && t->kind == TY_FLOAT && t->bits == 32) emit(cg, "f");
}

static bool is_string_type(Node *n) {
    return n->type && n->type->kind == TY_STRING;
}

static bool is_integral_t(TypeInfo *t) {
    return t && (t->kind == TY_INT || t->kind == TY_BOOL || t->kind == TY_ENUM);
}

/* Pointer-like destination types need an explicit cast when the source is
 * a different pointer type (class upcasts, void*, char* vs string, ...). */
static void gen_conv(CodeGen *cg, TypeInfo *dst, Node *src) {
    bool dst_ptr = dst && (dst->kind == TY_CLASS || dst->kind == TY_POINTER ||
                           dst->kind == TY_FUNC || dst->kind == TY_STRING);
    if (dst_ptr && src->type && src->type->kind != TY_NULL &&
        src->type->kind != TY_UNKNOWN && !type_same(dst, src->type) &&
        !(dst->kind == TY_STRING && src->type->kind == TY_STRING)) {
        emitf(cg, "((%s)", decl_str(cg, dst, ""));
        gen_expr(cg, src);
        emit(cg, ")");
        return;
    }
    gen_expr(cg, src);
}

static void gen_args(CodeGen *cg, Param *params, NodeList *args, bool lead) {
    Param *p = params;
    for (NodeList *a = args; a; a = a->next) {
        if (lead || a != args) emit(cg, ", ");
        gen_conv(cg, p ? p->type : NULL, a->node);
        if (p) p = p->next;
    }
}

static void gen_nn_open(CodeGen *cg, Node *obj, TypeInfo *as, int line) {
    /* ((T)tz_nn((void *)(obj), line)) when checks are on and obj may be null */
    if (cg->checks && obj->kind != NK_THIS) {
        cg->uses_checks = true;
        emitf(cg, "((%s)tz_nn((void *)(", decl_str(cg, as, ""));
        gen_expr(cg, obj);
        emitf(cg, "), %d))", line);
    } else {
        emit(cg, "(");
        gen_expr(cg, obj);
        emit(cg, ")");
    }
}

static void gen_member(CodeGen *cg, Node *n) {
    if (n->member.is_static) {
        emitf(cg, "tz_%s__%s", n->member.owner, n->member.field);
        return;
    }
    Node *obj = n->member.object;
    TypeInfo *ot = obj->type;
    if (n->member.hops == -2) {                       /* string.length */
        emit(cg, "((long long)strlen(");
        gen_expr(cg, obj);
        emit(cg, "))");
        return;
    }
    if (n->member.hops == -4) {                       /* Map / Set .length */
        emitf(cg, "tz_Map_%d_len(", map_index(cg, ot));
        gen_expr(cg, obj);
        emitf(cg, ", %d)", n->line);
        return;
    }
    if (n->member.hops == -3) {                       /* List.length  */
        emitf(cg, "tz_List_%d_len(", list_index(cg, ot->elem_type));
        gen_expr(cg, obj);
        emitf(cg, ", %d)", n->line);
        return;
    }
    if (ot && (ot->kind == TY_CLASS || ot->kind == TY_POINTER)) {
        gen_nn_open(cg, obj, ot, n->line);
        emit(cg, "->");
        for (int i = 0; i < n->member.hops; i++) emit(cg, "_b.");
    } else {
        gen_expr(cg, obj);
        emit(cg, ".");
    }
    emit_ident(cg, n->member.field);
}

static TypeInfo *slot_type(CodeGen *cg, Method *m) {
    Param *self = param_new(cg->arena, "self", type_pointer(cg->arena, &ty_void));
    self->next = m->params;
    return type_func(cg->arena, self, m->ret_type);
}

static void gen_list_call(CodeGen *cg, Node *n) {
    static const char *NM[] = { "", "push", "pop", "at", "at", "insert", "remove", "clear" };
    TypeInfo *lt = n->mcall.object->type;
    TypeInfo *et = lt->elem_type;
    int id = n->mcall.builtin;
    const char *L = tfmt(cg, "tz_List_%d", list_index(cg, et));
    Node *a1 = n->mcall.args ? n->mcall.args->node : NULL;
    Node *a2 = n->mcall.args && n->mcall.args->next ? n->mcall.args->next->node : NULL;
    if (id == 3) emit(cg, "(*");
    if (id == 4) emit(cg, "(*");
    emitf(cg, "%s_%s(", L, NM[id]);
    gen_expr(cg, n->mcall.object);
    switch (id) {
        case 1: emit(cg, ", "); gen_conv(cg, et, a1); break;
        case 3: case 6: emit(cg, ", "); gen_expr(cg, a1); break;
        case 4: emit(cg, ", "); gen_expr(cg, a1); break;
        case 5: emit(cg, ", "); gen_expr(cg, a1); emit(cg, ", "); gen_conv(cg, et, a2); break;
        default: break;
    }
    emitf(cg, ", %d)", n->line);
    if (id == 3) emit(cg, ")");
    if (id == 4) { emit(cg, " = "); gen_conv(cg, et, a2); emit(cg, ")"); }
}

static void gen_map_call(CodeGen *cg, Node *n) {
    TypeInfo *mt = n->mcall.object->type;
    int id = n->mcall.builtin;
    const char *M = tfmt(cg, "tz_Map_%d", map_index(cg, mt));
    Node *a1 = n->mcall.args ? n->mcall.args->node : NULL;
    Node *a2 = n->mcall.args && n->mcall.args->next ? n->mcall.args->next->node : NULL;
    const char *fn = "";
    switch (id) {
        case 11: fn = "ref"; break;     case 12: fn = "get"; break;
        case 13: fn = "get_or"; break;  case 14: case 22: fn = "has"; break;
        case 15: case 23: fn = "remove"; break;
        case 16: case 24: fn = "clear"; break;
        case 21: fn = "insert"; break;
        default: break;
    }
    if (id == 17 || id == 18 || id == 25) {        /* keys() / values() / items() -> List */
        TypeInfo *et = id == 18 ? mt->elem_type : mt->key_type;
        const char *L = tfmt(cg, "tz_List_%d", list_index(cg, et));
        emitf(cg, "({ %s *_m = ", M);
        gen_expr(cg, n->mcall.object);
        emitf(cg, "; %s *_l = %s_new(); long long _n = %s_len(_m, %d); "
                  "for (long long _i = 0; _i < _n; _i++) %s_push(_l, _m->%s[_i], %d); _l; })",
              L, L, M, n->line, L, id == 18 ? "vals" : "keys", n->line);
        return;
    }
    if (id == 11) emit(cg, "(void)(*");
    emitf(cg, "%s_%s(", M, fn);
    gen_expr(cg, n->mcall.object);
    if (a1) { emit(cg, ", "); gen_conv(cg, mt->key_type, a1); }
    if (id == 13) { emit(cg, ", "); gen_conv(cg, mt->elem_type, a2); }
    emitf(cg, ", %d)", n->line);
    if (id == 11) { emit(cg, " = "); gen_conv(cg, mt->elem_type, a2); emit(cg, ")"); }
}

static void gen_mcall(CodeGen *cg, Node *n) {
    Method *m = n->mcall.resolved;
    Node *obj = n->mcall.object;
    if (n->mcall.builtin >= 11) { gen_map_call(cg, n); return; }
    if (n->mcall.builtin) { gen_list_call(cg, n); return; }

    if (!m) {                                         /* struct callback field */
        gen_expr(cg, obj);
        emit(cg, obj->type && obj->type->kind == TY_POINTER ? "->" : ".");
        emit_ident(cg, n->mcall.method);
        emit(cg, "(");
        gen_args(cg, n->mcall.fparams, n->mcall.args, false);
        emit(cg, ")");
        return;
    }
    if (n->mcall.is_static_call) {
        emitf(cg, "tz_%s__%s(", m->owner, m->name);
        gen_args(cg, m->params, n->mcall.args, false);
        emit(cg, ")");
        return;
    }
    if (n->mcall.is_super_call) {
        emitf(cg, "tz_%s__%s((tz_%s *)self", m->owner, m->name, m->owner);
        gen_args(cg, m->params, n->mcall.args, true);
        emit(cg, ")");
        return;
    }

    /* virtual call through the vtable of the static type */
    Node *cls = class_of(cg, obj->type);
    Node *root = root_class(cls);
    bool pure = is_pure(obj);
    const char *vt = tfmt(cg, "((const struct tz_%s__vt *)((struct tz_%s *)",
                          cls->class_decl.name, root->class_decl.name);
    if (pure) {
        emit(cg, vt);
        if (cg->checks && obj->kind != NK_THIS) {
            cg->uses_checks = true;
            emit(cg, "tz_nn((void *)(");
            gen_expr(cg, obj);
            emitf(cg, "), %d)", n->line);
        } else {
            emit(cg, "(");
            gen_expr(cg, obj);
            emit(cg, ")");
        }
        emitf(cg, ")->_vt)->m_%s(", m->name);
        gen_expr(cg, obj);
    } else {
        cg->uses_checks = cg->uses_checks || cg->checks;
        emit(cg, "({ void *_o = ");
        if (cg->checks) {
            emit(cg, "tz_nn((void *)(");
            gen_expr(cg, obj);
            emitf(cg, "), %d); ", n->line);
        } else {
            emit(cg, "(void *)(");
            gen_expr(cg, obj);
            emit(cg, "); ");
        }
        emit(cg, vt);
        emitf(cg, "_o)->_vt)->m_%s(_o", m->name);
    }
    gen_args(cg, m->params, n->mcall.args, true);
    emit(cg, pure ? ")" : "); })");
}

static bool is_int_div(Node *n) {
    return (n->binary.op == BOP_DIV || n->binary.op == BOP_MOD) &&
           is_integral_t(n->binary.left->type) && is_integral_t(n->binary.right->type);
}

static bool nonzero_lit(Node *r) {
    return r->kind == NK_INT_LIT && r->int_lit.value != 0;
}

static void gen_expr(CodeGen *cg, Node *n) {
    switch (n->kind) {
        case NK_INT_LIT:
            if (n->int_lit.is_unsigned) emitf(cg, "%lluULL", (unsigned long long)n->int_lit.value);
            else if (n->int_lit.value == LLONG_MIN) emit(cg, "(-9223372036854775807LL-1)");
            else emitf(cg, "%lld", n->int_lit.value);
            break;
        case NK_FLOAT_LIT:  gen_float(cg, n->float_lit.value, n->type); break;
        case NK_STRING_LIT: emit_c_string(cg, n->string_lit.value); break;
        case NK_BOOL_LIT:   emit(cg, n->bool_lit.value ? "1" : "0"); break;
        case NK_NULL_LIT:   emit(cg, "NULL"); break;
        case NK_THIS:       emit(cg, "self"); break;
        case NK_IDENT:
            if (n->ident.cname) emit(cg, n->ident.cname);
            else emit_ident(cg, n->ident.name);
            break;

        case NK_ASSIGN: {
            Node *t = n->assign.target, *v = n->assign.value;
            int op = n->assign.op;
            if (op == BOP_NONE) {
                emit(cg, "(");
                gen_expr(cg, t);
                emit(cg, " = ");
                gen_conv(cg, t->type, v);
                emit(cg, ")");
            } else if (op == BOP_ADD && is_string_type(t) && is_string_type(v)) {
                cg->uses_heap = true;
                if (is_pure(t)) {
                    emit(cg, "(");
                    gen_expr(cg, t);
                    emit(cg, " = tz_concat(");
                    gen_expr(cg, t);
                    emit(cg, ", ");
                    gen_expr(cg, v);
                    emit(cg, "))");
                } else {
                    emit(cg, "({ const char **_p = &(");
                    gen_expr(cg, t);
                    emit(cg, "); *_p = tz_concat(*_p, ");
                    gen_expr(cg, v);
                    emit(cg, "); })");
                }
            } else {
                emit(cg, "(");
                gen_expr(cg, t);
                emitf(cg, " %s= ", binop_name((BinOp)op));
                if (cg->checks && (op == BOP_DIV || op == BOP_MOD) &&
                    is_integral_t(t->type) && is_integral_t(v->type) &&
                    !nonzero_lit(v)) {
                    cg->uses_checks = true;
                    emit(cg, "tz_nz(");
                    gen_expr(cg, v);
                    emitf(cg, ", %d)", n->line);
                } else {
                    gen_expr(cg, v);
                }
                emit(cg, ")");
            }
            break;
        }

        case NK_BINARY: {
            Node *L = n->binary.left, *R = n->binary.right;
            BinOp op = n->binary.op;
            if (is_string_type(L) && is_string_type(R) && op == BOP_ADD) {
                cg->uses_heap = true;
                emit(cg, "tz_concat(");
                gen_expr(cg, L); emit(cg, ", "); gen_expr(cg, R);
                emit(cg, ")");
                break;
            }
            if (is_string_type(L) && is_string_type(R)) {
                emit(cg, "(strcmp(");
                gen_expr(cg, L); emit(cg, ", "); gen_expr(cg, R);
                emitf(cg, ") %s 0)", binop_name(op));
                break;
            }
            bool cmp = op == BOP_EQ || op == BOP_NE || op == BOP_LT ||
                       op == BOP_LE || op == BOP_GT || op == BOP_GE;
            bool ptrcmp = cmp && L->type && R->type && L->type->kind != TY_UNKNOWN &&
                          (L->type->kind == TY_CLASS || L->type->kind == TY_FUNC ||
                           R->type->kind == TY_CLASS || R->type->kind == TY_FUNC) &&
                          L->type->kind != TY_NULL && R->type->kind != TY_NULL;
            emit(cg, "(");
            if (ptrcmp) emit(cg, "(void *)");
            gen_expr(cg, L);
            emitf(cg, " %s ", binop_name(op));
            if (ptrcmp) emit(cg, "(void *)");
            if (cg->checks && is_int_div(n) && !nonzero_lit(R)) {
                cg->uses_checks = true;
                emit(cg, "tz_nz(");
                gen_expr(cg, R);
                emitf(cg, ", %d)", n->line);
            } else {
                gen_expr(cg, R);
            }
            emit(cg, ")");
            break;
        }

        case NK_UNARY: {
            Node *o = n->unary.operand;
            if (n->unary.op == UOP_DEREF) {
                TypeInfo *pt = o->type;
                if (pt && pt->kind == TY_STRING) pt = type_pointer(cg->arena, &ty_char);
                emit(cg, "(*");
                gen_nn_open(cg, o, pt, n->line);
                emit(cg, ")");
                break;
            }
            emitf(cg, "%s(", unaryop_name(n->unary.op));
            gen_expr(cg, o);
            emit(cg, ")");
            break;
        }

        case NK_CALL:
            if (n->call.cname) emit(cg, n->call.cname);     /* C binding: raw name */
            else               emit_ident(cg, n->call.name);
            emit(cg, "(");
            gen_args(cg, n->call.params, n->call.args, false);
            emit(cg, ")");
            break;

        case NK_MCALL: gen_mcall(cg, n); break;

        case NK_NEW:
            cg->uses_heap = true;
            emitf(cg, "tz_%s__new(", n->new_expr.name);
            gen_args(cg, n->new_expr.ctor ? n->new_expr.ctor->params : NULL,
                     n->new_expr.args, false);
            emit(cg, ")");
            break;

        case NK_LIST_NEW: {
            if (n->list_new.ltype->kind == TY_MAP) {
                TypeInfo *mt = n->list_new.ltype;
                const char *M = tfmt(cg, "tz_Map_%d", map_index(cg, mt));
                emitf(cg, "({ %s *_l = %s_new(); ", M, M);
                for (NodeList *a = n->list_new.items; a; a = a->next) {
                    emitf(cg, "%s_insert(_l, ", M);
                    gen_conv(cg, mt->key_type, a->node);
                    emitf(cg, ", %d); ", n->line);
                }
                emit(cg, "_l; })");
                break;
            }
            TypeInfo *et = n->list_new.ltype->elem_type;
            const char *L = tfmt(cg, "tz_List_%d", list_index(cg, et));
            emitf(cg, "({ %s *_l = %s_new(); ", L, L);
            for (NodeList *a = n->list_new.items; a; a = a->next) {
                emitf(cg, "%s_push(_l, ", L);
                gen_conv(cg, et, a->node);
                emitf(cg, ", %d); ", n->line);
            }
            emit(cg, "_l; })");
            break;
        }

        case NK_CTOR_CALL: {
            Method *c = n->ctor_call.ctor;
            if (!c) { emit(cg, "(void)0"); break; }
            emitf(cg, "tz_%s__init((tz_%s *)self", c->owner, c->owner);
            gen_args(cg, c->params, n->ctor_call.args, true);
            emit(cg, ")");
            break;
        }

        case NK_INDEX: {
            Node *a = n->index.array, *ix = n->index.index;
            TypeInfo *at = a->type;
            if (at && at->kind == TY_MAP) {
                emitf(cg, n->as_lvalue ? "(*tz_Map_%d_ref(" : "tz_Map_%d_get(", map_index(cg, at));
                gen_expr(cg, a);
                emit(cg, ", ");
                gen_conv(cg, at->key_type, ix);
                emitf(cg, n->as_lvalue ? ", %d))" : ", %d)", n->line);
                break;
            }
            if (at && at->kind == TY_LIST) {
                emitf(cg, "(*tz_List_%d_at(", list_index(cg, at->elem_type));
                gen_expr(cg, a);
                emit(cg, ", ");
                gen_expr(cg, ix);
                emitf(cg, ", %d))", n->line);
                break;
            }
            gen_expr(cg, a);
            emit(cg, "[");
            if (cg->checks && at && at->kind == TY_ARRAY && at->array_size > 0 &&
                ix->kind != NK_INT_LIT) {
                cg->uses_checks = true;
                emit(cg, "tz_bc(");
                gen_expr(cg, ix);
                emitf(cg, ", %d, %d)", at->array_size, n->line);
            } else {
                gen_expr(cg, ix);
            }
            emit(cg, "]");
            break;
        }

        case NK_MEMBER: gen_member(cg, n); break;

        case NK_CAST: {
            TypeInfo *to = n->cast.to;
            if (to->kind == TY_BOOL) {
                emit(cg, "((");
                gen_expr(cg, n->cast.expr);
                emit(cg, ") != 0)");
            } else {
                emitf(cg, "((%s)(", decl_str(cg, to, ""));
                gen_expr(cg, n->cast.expr);
                emit(cg, "))");
            }
            break;
        }

        case NK_SIZEOF:
            if (n->size_of.of_type) {
                TypeInfo *t = n->size_of.of_type;
                if (t->kind == TY_CLASS) emitf(cg, "sizeof(tz_%s)", t->struct_name);
                else emitf(cg, "sizeof(%s)", decl_str(cg, t, ""));
            } else {
                emit(cg, "sizeof(");
                gen_expr(cg, n->size_of.expr);
                emit(cg, ")");
            }
            break;

        case NK_TERNARY:
            emit(cg, "(");
            gen_expr(cg, n->ternary.cond);
            emit(cg, " ? ");
            gen_conv(cg, n->type, n->ternary.then_e);
            emit(cg, " : ");
            gen_conv(cg, n->type, n->ternary.else_e);
            emit(cg, ")");
            break;

        case NK_INCDEC:
            emit(cg, "(");
            if (n->incdec.prefix) emit(cg, n->incdec.is_inc ? "++" : "--");
            gen_expr(cg, n->incdec.operand);
            if (!n->incdec.prefix) emit(cg, n->incdec.is_inc ? "++" : "--");
            emit(cg, ")");
            break;

        case NK_STRUCT_LIT: {
            Node *sd = find_decl(cg, NK_STRUCT_DECL, n->struct_lit.name);
            emitf(cg, "((tz_%s){", n->struct_lit.name);
            if (!n->struct_lit.inits) emit(cg, "0");
            for (FieldInit *fi = n->struct_lit.inits; fi; fi = fi->next) {
                Field *f = sd ? sd->struct_decl.fields : NULL;
                while (f && strcmp(f->name, fi->name)) f = f->next;
                emit(cg, ".");
                emit_ident(cg, fi->name);
                emit(cg, " = ");
                gen_conv(cg, f ? f->type : NULL, fi->value);
                if (fi->next) emit(cg, ", ");
            }
            emit(cg, "})");
            break;
        }

        default:
            emit(cg, "0 /* unsupported */");
            break;
    }
}

/* An assignment used as a statement needs no surrounding parentheses. */
static void gen_expr_stmt(CodeGen *cg, Node *e) {
    if (e->kind == NK_ASSIGN && e->assign.op == BOP_NONE) {
        gen_expr(cg, e->assign.target);
        emit(cg, " = ");
        gen_conv(cg, e->assign.target->type, e->assign.value);
    } else {
        gen_expr(cg, e);
    }
}

/* ─── Statements ──────────────────────────────────────────────────────── */
static bool terminates(Node *n) {
    return n->kind == NK_RETURN || n->kind == NK_BREAK ||
           n->kind == NK_CONTINUE;
}

static void gen_block(CodeGen *cg, Node *blk) {
    emit(cg, "{\n");
    cg->indent++;
    for (NodeList *it = blk->block.stmts; it; it = it->next) {
        gen_stmt(cg, it->node);
        if (terminates(it->node)) break;          /* rest is unreachable */
    }
    cg->indent--;
    indent(cg);
    emit(cg, "}");
}

static void gen_initialiser(CodeGen *cg, TypeInfo *vt, Node *init) {
    if (init && init->kind == NK_ARRAY_INIT) {
        TypeInfo *et = vt && vt->kind == TY_ARRAY ? vt->elem_type : NULL;
        emit(cg, "{");
        for (NodeList *e = init->array_init.elements; e; e = e->next) {
            e->node = simplify(cg, e->node);
            gen_conv(cg, et, e->node);
            if (e->next) emit(cg, ", ");
        }
        emit(cg, "}");
    } else if (init) {
        gen_conv(cg, vt, init);
    } else {
        gen_zero_init(cg, vt);
    }
}

/* inline: part of a 'for' header (no indent, no ';'). */
static void gen_var_decl(CodeGen *cg, Node *n, bool inl) {
    TypeInfo *vt = n->var_decl.var_type ? n->var_decl.var_type : n->type;
    Node *init = n->var_decl.init;
    if (n->type && n->type->kind == TY_ARRAY) vt = n->type;   /* sized by sema */
    if (init && init->kind != NK_ARRAY_INIT) init = n->var_decl.init = simplify(cg, init);

    if (!inl) indent(cg);
    emit(cg, var_decl_str(cg, vt, ident_str(cg, n->var_decl.name),
                          n->var_decl.quals, true));
    emit(cg, " = ");
    gen_initialiser(cg, vt, init);
    if (!inl) emit(cg, ";\n");
}

static void gen_pathi(CodeGen *cg, Node *n) {
    char kinds[64];
    int count = 0;
    const char *err;
    char *fmt = fmt_rewrite(cg->arena, n->pathi.fmt->string_lit.value, kinds,
                            (int)sizeof kinds, &count, &err);

    indent(cg);
    emit(cg, "printf(");
    emit_c_string(cg, fmt ? fmt : "");
    int i = 0;
    for (NodeList *a = n->pathi.args; a; a = a->next, i++) {
        a->node = simplify(cg, a->node);
        emit(cg, ", ");
        switch (i < count ? kinds[i] : 's') {
            case 'i': emit(cg, "(long long)"); break;
            case 'f': emit(cg, "(double)");    break;
            case 'c': emit(cg, "(int)");       break;
            default: break;
        }
        gen_expr(cg, a->node);
    }
    emit(cg, ");\n");
}

/* Constant truth value of a (simplified) condition, if it has one. */
static bool const_cond(Node *c, bool *val) {
    if (!is_num_lit(c)) return false;
    *val = fval(c) != 0.0;
    return true;
}

static void gen_if(CodeGen *cg, Node *n) {
    bool emitted = false;
    for (IfBranch *br = n->if_stmt.branches; br; br = br->next) {
        bool truth = true, constant = true;
        if (br->cond) {
            br->cond = simplify(cg, br->cond);
            constant = const_cond(br->cond, &truth);
        }
        if (constant && !truth) continue;                 /* dead branch    */

        if (!br->cond || constant) {                      /* else / always  */
            if (emitted) emit(cg, " else ");
            else         indent(cg);
            gen_block(cg, br->body);
            emitted = true;
            break;                                        /* rest unreachable */
        }
        if (emitted) emit(cg, " else if (");
        else { indent(cg); emit(cg, "if ("); }
        gen_expr(cg, br->cond);
        emit(cg, ") ");
        gen_block(cg, br->body);
        emitted = true;
    }
    if (emitted) emit(cg, "\n");
}

static void gen_switch(CodeGen *cg, Node *n) {
    n->switch_stmt.subject = simplify(cg, n->switch_stmt.subject);
    indent(cg);
    emit(cg, "switch (");
    gen_expr(cg, n->switch_stmt.subject);
    emit(cg, ") {\n");
    int saved_break = cg->break_base;
    cg->break_base = cg->try_depth;
    for (SwitchCase *c = n->switch_stmt.cases; c; c = c->next) {
        indent(cg);
        if (c->is_default) emit(cg, "default:");
        for (NodeList *l = c->labels; l; l = l->next) {
            emit(cg, "case ");
            gen_expr(cg, l->node);
            emit(cg, ":");
            if (l->next) emit(cg, " ");
        }
        emit(cg, " ");
        gen_block(cg, c->body);
        NodeList *last = c->body->block.stmts;
        while (last && last->next) last = last->next;
        if (!last || !terminates(last->node)) emit(cg, " break;");
        emit(cg, "\n");
    }
    cg->break_base = saved_break;
    indent(cg);
    emit(cg, "}\n");
}

/* #line makes gdb, the C compiler and stack traces talk about the .tz file. */
static void emit_line(CodeGen *cg, Node *n) {
    if (!cg->lines || n->line <= 0) return;
    const char *f = n->file ? n->file : cg->filename;
    emitf(cg, "#line %d \"", n->line);
    for (const char *p = f ? f : "?"; *p; p++) {
        if (*p == '"' || *p == '\\') emit(cg, "\\");
        if ((unsigned char)*p >= 0x20) emit_n(cg, p, 1);
    }
    emit(cg, "\"\n");
}

static void gen_stmt(CodeGen *cg, Node *n) {
    if (n->kind != NK_BLOCK) emit_line(cg, n);
    switch (n->kind) {
        case NK_VAR_DECL: gen_var_decl(cg, n, false); break;
        case NK_BLOCK:    indent(cg); gen_block(cg, n); emit(cg, "\n"); break;
        case NK_IF:       gen_if(cg, n); break;
        case NK_SWITCH:   gen_switch(cg, n); break;

        case NK_WHILE: {
            n->while_stmt.cond = simplify(cg, n->while_stmt.cond);
            bool truth;
            if (const_cond(n->while_stmt.cond, &truth) && !truth) break;
            indent(cg);
            emit(cg, "while (");
            gen_expr(cg, n->while_stmt.cond);
            emit(cg, ") ");
            int sb = cg->break_base, sc = cg->cont_base;
            cg->break_base = cg->cont_base = cg->try_depth;
            gen_block(cg, n->while_stmt.body);
            cg->break_base = sb; cg->cont_base = sc;
            emit(cg, "\n");
            break;
        }

        case NK_DO_WHILE: {
            n->while_stmt.cond = simplify(cg, n->while_stmt.cond);
            indent(cg);
            emit(cg, "do ");
            int sb = cg->break_base, sc = cg->cont_base;
            cg->break_base = cg->cont_base = cg->try_depth;
            gen_block(cg, n->while_stmt.body);
            cg->break_base = sb; cg->cont_base = sc;
            emit(cg, " while (");
            gen_expr(cg, n->while_stmt.cond);
            emit(cg, ");\n");
            break;
        }

        case NK_FOR: {
            indent(cg);
            emit(cg, "for (");
            Node *in = n->for_stmt.init;
            if (in) {
                if (in->kind == NK_VAR_DECL) gen_var_decl(cg, in, true);
                else {
                    in->expr_stmt.expr = simplify(cg, in->expr_stmt.expr);
                    gen_expr_stmt(cg, in->expr_stmt.expr);
                }
            }
            emit(cg, "; ");
            if (n->for_stmt.cond) {
                n->for_stmt.cond = simplify(cg, n->for_stmt.cond);
                gen_expr(cg, n->for_stmt.cond);
            }
            emit(cg, "; ");
            if (n->for_stmt.update) {
                n->for_stmt.update = simplify(cg, n->for_stmt.update);
                gen_expr_stmt(cg, n->for_stmt.update);
            }
            emit(cg, ") ");
            int sb = cg->break_base, sc = cg->cont_base;
            cg->break_base = cg->cont_base = cg->try_depth;
            gen_block(cg, n->for_stmt.body);
            cg->break_base = sb; cg->cont_base = sc;
            emit(cg, "\n");
            break;
        }

        case NK_FOREACH: {
            static int fe_id = 0;
            int id = fe_id++;
            Node *coll = n->foreach_stmt.coll = simplify(cg, n->foreach_stmt.coll);
            TypeInfo *ct = coll->type, *et = n->foreach_stmt.elem;
            const char *var = decl_str(cg, et, ident_str(cg, n->foreach_stmt.name));
            bool cast = et && (et->kind == TY_CLASS || et->kind == TY_POINTER);
            indent(cg);
            emit(cg, "{ ");
            if (ct->kind == TY_LIST) {
                int li = list_index(cg, ct->elem_type);
                emitf(cg, "tz_List_%d *_c%d = ", li, id);
                gen_expr(cg, coll);
                emitf(cg, "; long long _i%d; for (_i%d = 0; _i%d < tz_List_%d_len(_c%d, %d); _i%d++) { ",
                      id, id, id, li, id, n->line, id);
                emitf(cg, "%s = ", var);
                if (cast) emitf(cg, "(%s)", decl_str(cg, et, ""));
                emitf(cg, "*tz_List_%d_at(_c%d, _i%d, %d);\n", li, id, id, n->line);
            } else if (ct->kind == TY_MAP) {
                int mi = map_index(cg, ct);
                emitf(cg, "tz_Map_%d *_c%d = ", mi, id);
                gen_expr(cg, coll);
                emitf(cg, "; long long _i%d; for (_i%d = 0; _i%d < tz_Map_%d_len(_c%d, %d); _i%d++) { ",
                      id, id, id, mi, id, n->line, id);
                emitf(cg, "%s = _c%d->keys[_i%d];\n", var, id, id);
            } else if (ct->kind == TY_STRING) {
                emitf(cg, "const char *_c%d = ", id);
                gen_expr(cg, coll);
                emitf(cg, "; long long _i%d; for (_i%d = 0; _c%d[_i%d]; _i%d++) { ", id, id, id, id, id);
                emitf(cg, "%s = _c%d[_i%d];\n", var, id, id);
            } else {
                emitf(cg, "long long _i%d; for (_i%d = 0; _i%d < %d; _i%d++) { ", id, id, id, ct->array_size, id);
                emitf(cg, "%s = ", var);
                gen_expr(cg, coll);
                emitf(cg, "[_i%d];\n", id);
            }
            int sb = cg->break_base, sc = cg->cont_base;
            cg->break_base = cg->cont_base = cg->try_depth;
            cg->indent++;
            gen_block(cg, n->foreach_stmt.body);
            cg->indent--;
            cg->break_base = sb; cg->cont_base = sc;
            emit(cg, " } }\n");
            break;
        }

        case NK_RETURN:
            indent(cg);
            if (cg->try_depth > 0) {        /* leave every active try frame */
                emit(cg, "{ ");
                if (n->ret.value) {
                    n->ret.value = simplify(cg, n->ret.value);
                    emitf(cg, "%s = ", decl_str(cg, cg->ret_type, "_rv"));
                    gen_conv(cg, cg->ret_type, n->ret.value);
                    emit(cg, "; ");
                }
                emit(cg, "tz_top = _f1.prev; return");
                if (n->ret.value) emit(cg, " _rv");
                emit(cg, "; }\n");
                break;
            }
            emit(cg, "return");
            if (n->ret.value) {
                n->ret.value = simplify(cg, n->ret.value);
                emit(cg, " ");
                gen_conv(cg, cg->ret_type, n->ret.value);
            }
            emit(cg, ";\n");
            break;

        case NK_BREAK:
            indent(cg);
            if (cg->try_depth > cg->break_base)
                emitf(cg, "tz_top = _f%d.prev; ", cg->break_base + 1);
            emit(cg, "break;\n");
            break;
        case NK_CONTINUE:
            indent(cg);
            if (cg->try_depth > cg->cont_base)
                emitf(cg, "tz_top = _f%d.prev; ", cg->cont_base + 1);
            emit(cg, "continue;\n");
            break;

        case NK_THROW:
            cg->uses_throw = true;
            n->throw_stmt.value = simplify(cg, n->throw_stmt.value);
            indent(cg);
            emitf(cg, "tz_raise(%d, 0, ", n->line);
            gen_expr(cg, n->throw_stmt.value);
            emit(cg, ");\n");
            break;

        case NK_TRY: {
            cg->uses_throw = true;
            int d = cg->try_depth + 1;
            indent(cg);
            emitf(cg, "{ tz_Frame _f%d; _f%d.prev = tz_top; tz_top = &_f%d;\n", d, d, d);
            cg->indent++;
            indent(cg);
            emitf(cg, "if (setjmp(_f%d.jb) == 0) {\n", d);
            cg->indent++;
            cg->try_depth++;
            for (NodeList *it = n->try_stmt.body->block.stmts; it; it = it->next) {
                gen_stmt(cg, it->node);
                if (terminates(it->node)) break;
            }
            cg->try_depth--;
            indent(cg);
            emitf(cg, "tz_top = _f%d.prev;\n", d);
            cg->indent--;
            indent(cg);
            emitf(cg, "} else {\n");
            cg->indent++;
            indent(cg);
            emitf(cg, "tz_top = _f%d.prev;\n", d);
            if (n->try_stmt.catch_name) {
                indent(cg);
                emitf(cg, "const char *tz_%s = _f%d.msg; (void)tz_%s;\n",
                      n->try_stmt.catch_name, d, n->try_stmt.catch_name);
            }
            for (NodeList *it = n->try_stmt.catch_body->block.stmts; it; it = it->next) {
                gen_stmt(cg, it->node);
                if (terminates(it->node)) break;
            }
            cg->indent--;
            indent(cg);
            emit(cg, "}\n");
            cg->indent--;
            indent(cg);
            emit(cg, "}\n");
            break;
        }
        case NK_PATHI:    gen_pathi(cg, n); break;

        case NK_EXPR_STMT:
            n->expr_stmt.expr = simplify(cg, n->expr_stmt.expr);
            indent(cg);
            if (n->expr_stmt.expr->kind == NK_INDEX || n->expr_stmt.expr->kind == NK_MEMBER)
                emit(cg, "(void)");              /* a bare read: silence -Wunused-value */
            gen_expr_stmt(cg, n->expr_stmt.expr);
            emit(cg, ";\n");
            break;

        default:
            break;
    }
}

/* ─── Functions and methods ───────────────────────────────────────────── */
static void gen_func_header(CodeGen *cg, Node *f) {
    bool is_main = !strcmp(f->func_decl.name, "main");
    TypeInfo *rt = f->func_decl.ret_type ? f->func_decl.ret_type : &ty_void;
    if (f->func_decl.is_static) emit(cg, "static ");
    if (is_main) { emit(cg, "int main(int argc, char **argv)"); return; }
    emit(cg, decl_str(cg, rt, tfmt(cg, "%s(%s)", ident_str(cg, f->func_decl.name),
                                   params_str(cg, f->func_decl.params, true, NULL))));
}

static const char *method_cname(CodeGen *cg, Node *c, Method *m) {
    return m->is_ctor ? tfmt(cg, "tz_%s__init", c->class_decl.name)
                      : tfmt(cg, "tz_%s__%s", c->class_decl.name, m->name);
}

static void gen_method_header(CodeGen *cg, Node *c, Method *m) {
    const char *self = m->is_static ? NULL
                     : tfmt(cg, "tz_%s *self%s", c->class_decl.name,
                            m->params ? ", " : "");
    const char *ps = params_str(cg, m->params, true, self);
    if (m->is_ctor) { emitf(cg, "void %s(%s)", method_cname(cg, c, m), ps); return; }
    TypeInfo *rt = m->ret_type ? m->ret_type : &ty_void;
    emit(cg, decl_str(cg, rt, tfmt(cg, "%s(%s)", method_cname(cg, c, m), ps)));
}

static void gen_method_body(CodeGen *cg, Node *c, Method *m) {
    Node *saved_c = cg->cur_class;
    TypeInfo *saved_r = cg->ret_type;
    cg->try_depth = cg->break_base = cg->cont_base = 0;
    cg->cur_class = c;
    cg->ret_type = m->is_ctor ? &ty_void : (m->ret_type ? m->ret_type : &ty_void);

    emit(cg, "\n");
    gen_method_header(cg, c, m);
    emit(cg, " {\n");
    cg->indent++;
    NodeList *first = m->body->block.stmts;
    bool explicit_super = first && first->node->kind == NK_EXPR_STMT &&
                          first->node->expr_stmt.expr->kind == NK_CTOR_CALL;
    if (m->is_ctor && !explicit_super && c->class_decl.parent_decl) {
        Method *pc = eff_ctor(c->class_decl.parent_decl);
        if (pc) {
            indent(cg);
            emitf(cg, "tz_%s__init((tz_%s *)self);\n", pc->owner, pc->owner);
        }
    }
    for (NodeList *it = m->body->block.stmts; it; it = it->next) {
        gen_stmt(cg, it->node);
        if (terminates(it->node)) break;
    }
    cg->indent--;
    emit(cg, "}\n");
    cg->ret_type = saved_r;
    cg->cur_class = saved_c;
}

static void gen_new_function(CodeGen *cg, Node *c) {
    const char *name = c->class_decl.name;
    Method *ctor = c->class_decl.ctor;
    emitf(cg, "\nstatic tz_%s *tz_%s__new(%s) {\n", name, name,
          params_str(cg, ctor ? ctor->params : NULL, true, NULL));
    emitf(cg, "    tz_%s *self = (tz_%s *)tz_alloc_obj(sizeof(tz_%s));\n",
          name, name, name);
    emit(cg, "    self->");
    for (int i = 0; i < class_depth(c); i++) emit(cg, "_b.");
    emitf(cg, "_vt = (const void *)&tz_%s__vtable;\n", name);
    Method *ec = eff_ctor(c);
    if (ec) {
        emitf(cg, "    tz_%s__init((tz_%s *)self", ec->owner, ec->owner);
        if (ctor)
            for (Param *p = ctor->params; p; p = p->next)
                emitf(cg, ", tz_%s", p->name);
        emit(cg, ");\n");
    }
    emit(cg, "    return self;\n}\n");
}

/* ─── Types: struct / union / class / enum ────────────────────────────── */
static Field *decl_fields(Node *d) {
    return d->kind == NK_CLASS_DECL ? d->class_decl.fields : d->struct_decl.fields;
}
static const char *decl_name(Node *d) {
    return d->kind == NK_CLASS_DECL ? d->class_decl.name : d->struct_decl.name;
}

static void gen_type_body(CodeGen *cg, Node *d) {
    bool is_class = d->kind == NK_CLASS_DECL;
    bool is_union = !is_class && d->struct_decl.is_union;
    emitf(cg, "%s tz_%s {\n", is_union ? "union" : "struct", decl_name(d));
    cg->indent++;
    int count = 0;
    if (is_class) {
        indent(cg);
        if (d->class_decl.parent_decl)
            emitf(cg, "struct tz_%s _b;\n", d->class_decl.parent_decl->class_decl.name);
        else
            emit(cg, "const void *_vt;\n");
        count++;
    }
    for (Field *f = decl_fields(d); f; f = f->next) {
        if (is_class && f->is_static) continue;
        indent(cg);
        emit(cg, decl_str(cg, f->type, ident_str(cg, f->name)));
        emit(cg, ";\n");
        count++;
    }
    if (!count) { indent(cg); emit(cg, "char _empty;\n"); }
    cg->indent--;
    emit(cg, "};\n\n");
}

/* Types that contain other types by value must be emitted after them. */
static const char *field_dep(Field *f) {
    TypeInfo *t = f->type;
    while (t && t->kind == TY_ARRAY) t = t->elem_type;
    return (t && t->kind == TY_STRUCT) ? t->struct_name : NULL;
}

static bool depends_on(Node *d, const char *name) {
    if (d->kind == NK_CLASS_DECL && d->class_decl.parent &&
        !strcmp(d->class_decl.parent, name))
        return true;
    for (Field *f = decl_fields(d); f; f = f->next) {
        if (d->kind == NK_CLASS_DECL && f->is_static) continue;
        const char *dep = field_dep(f);
        if (dep && !strcmp(dep, name)) return true;
    }
    return false;
}

static void gen_types_ordered(CodeGen *cg, Node *prog) {
    int total = 0;
    for (NodeList *it = prog->program.decls; it; it = it->next)
        if (it->node->kind == NK_STRUCT_DECL || it->node->kind == NK_CLASS_DECL) total++;
    if (!total) return;

    bool *done = arena_alloc(cg->arena, (size_t)total * sizeof(bool));
    Node **decl = arena_alloc(cg->arena, (size_t)total * sizeof(Node *));
    int k = 0;
    for (NodeList *it = prog->program.decls; it; it = it->next)
        if (it->node->kind == NK_STRUCT_DECL || it->node->kind == NK_CLASS_DECL) {
            decl[k] = it->node; done[k++] = false;
        }

    emit(cg, "/* types */\n");
    for (int i = 0; i < total; i++)
        emitf(cg, "typedef %s tz_%s tz_%s;\n",
              decl[i]->kind == NK_STRUCT_DECL && decl[i]->struct_decl.is_union
                  ? "union" : "struct", decl_name(decl[i]), decl_name(decl[i]));
    emit(cg, "\n");

    int emitted = 0;
    for (int pass = 0; pass < total && emitted < total; pass++) {
        for (int i = 0; i < total; i++) {
            if (done[i]) continue;
            bool ready = true;
            for (int j = 0; j < total && ready; j++)
                if (!done[j] && j != i && depends_on(decl[i], decl_name(decl[j])))
                    ready = false;
            if (ready) { gen_type_body(cg, decl[i]); done[i] = true; emitted++; }
        }
    }
    for (int i = 0; i < total; i++)             /* cycles: let cc complain */
        if (!done[i]) gen_type_body(cg, decl[i]);
}

static void gen_enums(CodeGen *cg, Node *prog) {
    for (NodeList *it = prog->program.decls; it; it = it->next) {
        Node *d = it->node;
        if (d->kind != NK_ENUM_DECL) continue;
        bool wide = false;
        for (Enumerator *e = d->enum_decl.items; e; e = e->next)
            if (e->resolved > INT_MAX || e->resolved < INT_MIN) wide = true;
        emitf(cg, "typedef %s tz_%s;  /* enum:", wide ? "long long" : "int",
              d->enum_decl.name);
        for (Enumerator *e = d->enum_decl.items; e; e = e->next)
            emitf(cg, " %s=%lld", e->name, e->resolved);
        emit(cg, " */\n");
    }
}

static void gen_vtable_types(CodeGen *cg, Node *prog) {
    for (NodeList *it = prog->program.decls; it; it = it->next) {
        Node *c = it->node;
        if (c->kind != NK_CLASS_DECL) continue;
        emitf(cg, "struct tz_%s__vt {\n    int _n;\n", c->class_decl.name);
        for (int i = 0; i < c->class_decl.nvslots; i++) {
            Method *m = c->class_decl.vslots[i];
            emitf(cg, "    %s;\n",
                  decl_str(cg, slot_type(cg, m), tfmt(cg, "m_%s", m->name)));
        }
        emit(cg, "};\n\n");
    }
}

static void gen_vtables(CodeGen *cg, Node *prog) {
    for (NodeList *it = prog->program.decls; it; it = it->next) {
        Node *c = it->node;
        if (c->kind != NK_CLASS_DECL) continue;
        emitf(cg, "static const struct tz_%s__vt tz_%s__vtable = { 0",
              c->class_decl.name, c->class_decl.name);
        for (int i = 0; i < c->class_decl.nvslots; i++) {
            Method *m = c->class_decl.vslots[i];
            if (m->is_abstract) continue;
            emitf(cg, ",\n    .m_%s = (%s)tz_%s__%s", m->name,
                  decl_str(cg, slot_type(cg, m), ""), m->owner, m->name);
        }
        emit(cg, " };\n");
    }
}

void codegen_init(CodeGen *cg, Arena *arena, const char *filename) {
    strbuf_init(&cg->out);
    cg->indent = 0;
    cg->arena = arena;
    cg->filename = filename;
    cg->gc = false;
    cg->uses_heap = false;
    cg->checks = true;
    cg->uses_checks = false;
    cg->uses_throw = false;
    cg->lines = false;
    strbuf_init(&cg->lists);
    cg->nlists = 0;
    cg->nmaps = 0;
    cg->try_depth = cg->break_base = cg->cont_base = 0;
    cg->prog = NULL;
    cg->ret_type = NULL;
    cg->cur_class = NULL;
}

/* Runtime support, inserted after the includes only if the program needs it. */
static const char RT_COMMON[] =
    "static const char *tz_concat(const char *a, const char *b) __attribute__((unused));\n"
    "static const char *tz_concat(const char *a, const char *b) {\n"
    "    size_t la = strlen(a), lb = strlen(b);\n"
    "    char *r = (char *)tz_alloc(la + lb + 1);\n"
    "    memcpy(r, a, la);\n"
    "    memcpy(r + la, b, lb + 1);\n"
    "    return r;\n"
    "}\n\n";

static const char RT_GROW[] =
    "static void *tz_grow(void *old, size_t oldsz, size_t newsz) __attribute__((unused));\n"
    "static void *tz_grow(void *old, size_t oldsz, size_t newsz) {\n"
    "    void *p = tz_alloc_obj(newsz);\n"
    "    if (old) memcpy(p, old, oldsz);\n"
    "    return p;\n"
    "}\n\n";

static const char RT_GC[] =
    "/* --- runtime: heap via the Boehm collector (compile with --no-gc to use malloc) --- */\n"
    "extern void *GC_malloc(size_t);\n"
    "extern void *GC_malloc_atomic(size_t);\n"
    "extern void GC_init(void);\n"
    "static void tz_gc_boot(void) __attribute__((constructor(101)));\n"
    "static void tz_gc_boot(void) { GC_init(); }\n"
    "static void *tz_alloc(size_t n) __attribute__((unused));\n"
    "static void *tz_alloc(size_t n) {\n"
    "    void *p = GC_malloc_atomic(n);\n"
    "    if (!p) { fputs(\"tamizhi: out of memory\\n\", stderr); exit(1); }\n"
    "    return p;\n"
    "}\n"
    "static void *tz_alloc_obj(size_t n) __attribute__((unused));\n"
    "static void *tz_alloc_obj(size_t n) {          /* zeroed, scanned for pointers */\n"
    "    void *p = GC_malloc(n);\n"
    "    if (!p) { fputs(\"tamizhi: out of memory\\n\", stderr); exit(1); }\n"
    "    return p;\n"
    "}\n";

static const char RT_MALLOC[] =
    "/* --- runtime: heap via malloc, never freed (memory returns at exit) --- */\n"
    "static void *tz_alloc(size_t n) __attribute__((unused));\n"
    "static void *tz_alloc(size_t n) {\n"
    "    void *p = malloc(n);\n"
    "    if (!p) { fputs(\"tamizhi: out of memory\\n\", stderr); exit(1); }\n"
    "    return p;\n"
    "}\n"
    "static void *tz_alloc_obj(size_t n) __attribute__((unused));\n"
    "static void *tz_alloc_obj(size_t n) {\n"
    "    void *p = calloc(1, n);\n"
    "    if (!p) { fputs(\"tamizhi: out of memory\\n\", stderr); exit(1); }\n"
    "    return p;\n"
    "}\n";

static const char RT_THROW[] =
    "/* --- runtime: exceptions (muyarchi / pidi / erivi) --- */\n"
    "typedef struct tz_Frame { jmp_buf jb; struct tz_Frame *prev; const char *msg; } tz_Frame;\n"
    "static tz_Frame *tz_top;\n"
    "static void tz_raise(int line, int runtime, const char *msg) __attribute__((noreturn, unused));\n"
    "static void tz_raise(int line, int runtime, const char *msg) {\n"
    "    if (tz_top) {\n"
    "        tz_Frame *f = tz_top;\n"
    "        size_t n = strlen(msg);\n"
    "        char *copy = (char *)malloc(n + 1);\n"
    "        if (copy) { memcpy(copy, msg, n + 1); msg = copy; }\n"
    "        f->msg = msg;\n"
    "        longjmp(f->jb, 1);\n"
    "    }\n"
    "    fflush(stdout);\n"
    "    fprintf(stderr, \"%s:%d: %s: %s\\n\", TZ_FILE, line,\n"
    "            runtime ? \"runtime error\" : \"uncaught error\", msg);\n"
    "    exit(1);\n"
    "}\n\n";

static const char RT_CHECKS[] =
    "/* --- runtime checks (compile with --no-checks to remove them) --- */\n"
    "static void tz_fail(int line, const char *msg, long long a, long long b)\n"
    "    __attribute__((noreturn, unused));\n"
    "static void tz_fail(int line, const char *msg, long long a, long long b) {\n"
    "    char buf[200];\n"
    "    snprintf(buf, sizeof buf, msg, a, b);\n"
    "    tz_raise(line, 1, buf);\n"
    "}\n"
    "static long long tz_bc(long long i, long long n, int line) __attribute__((unused));\n"
    "static long long tz_bc(long long i, long long n, int line) {\n"
    "    if (i < 0 || i >= n) tz_fail(line, \"index %lld is out of bounds for an array of %lld\", i, n);\n"
    "    return i;\n"
    "}\n"
    "static long long tz_nz(long long d, int line) __attribute__((unused));\n"
    "static long long tz_nz(long long d, int line) {\n"
    "    if (d == 0) tz_fail(line, \"division by zero\", 0, 0);\n"
    "    return d;\n"
    "}\n"
    "static void *tz_nn(void *p, int line) __attribute__((unused));\n"
    "static void *tz_nn(void *p, int line) {\n"
    "    if (!p) tz_fail(line, \"null reference (illai) used\", 0, 0);\n"
    "    return p;\n"
    "}\n\n";

static void insert_at(CodeGen *cg, size_t pos, const char *a, const char *b) {
    StrBuf nb;
    strbuf_init(&nb);
    strbuf_append_n(&nb, cg->out.data, pos);
    strbuf_append(&nb, a);
    strbuf_append(&nb, b);
    strbuf_append_n(&nb, cg->out.data + pos, cg->out.len - pos);
    strbuf_free(&cg->out);
    cg->out = nb;
}

/* Prototype for a C binding that no header describes. The declared Tamizhi
 * types must then match the C signature exactly. */
static void gen_extern_proto(CodeGen *cg, Node *d) {
    TypeInfo *rt = d->extern_func.ret_type ? d->extern_func.ret_type : &ty_void;
    emit(cg, decl_str(cg, rt, tfmt(cg, "%s(%s)", d->extern_func.cname,
                                   params_str(cg, d->extern_func.params, false, NULL))));
    emit(cg, ";\n");
}

/* C only accepts constant initialisers for globals. Anything else (a call, a
 * string concatenation, another variable) is assigned at start-up instead. */
static bool is_c_const(Node *n) {
    switch (n->kind) {
        case NK_INT_LIT: case NK_FLOAT_LIT: case NK_BOOL_LIT: case NK_STRING_LIT:
        case NK_NULL_LIT: case NK_SIZEOF:
            return true;
        case NK_UNARY:
            return n->unary.op != UOP_ADDR && n->unary.op != UOP_DEREF &&
                   is_c_const(n->unary.operand);
        case NK_BINARY:
            if (is_string_type(n->binary.left) || is_string_type(n->binary.right))
                return false;                       /* strcmp / concat */
            return is_c_const(n->binary.left) && is_c_const(n->binary.right);
        case NK_TERNARY:
            return is_c_const(n->ternary.cond) && is_c_const(n->ternary.then_e) &&
                   is_c_const(n->ternary.else_e);
        case NK_CAST:
            return n->cast.to->kind != TY_BOOL && is_c_const(n->cast.expr) &&
                   n->cast.to->kind != TY_STRING && n->cast.to->kind != TY_POINTER;
        default:
            return false;
    }
}

static bool init_is_static(Node *init) {
    if (!init) return true;
    if (init->kind == NK_ARRAY_INIT) {
        for (NodeList *e = init->array_init.elements; e; e = e->next)
            if (!is_c_const(e->node)) return false;
        return true;
    }
    return is_c_const(init);
}

/* A global (or static field) whose initialiser must run at start-up. */
typedef struct StartupInit {
    const char *cname;
    Node       *init;
    TypeInfo   *type;
    struct StartupInit *next;
} StartupInit;

static void gen_global(CodeGen *cg, TypeInfo *vt, const char *cname, Node *init,
                       int quals, StartupInit ***tail) {
    if (init) {
        if (init->kind == NK_ARRAY_INIT) {
            for (NodeList *e = init->array_init.elements; e; e = e->next)
                e->node = simplify(cg, e->node);
        } else {
            init = simplify(cg, init);
        }
    }
    bool late = init && !init_is_static(init);
    emit(cg, var_decl_str(cg, vt, cname, quals, !late));
    emit(cg, " = ");
    if (late) {
        gen_zero_init(cg, vt);
        StartupInit *si = arena_alloc(cg->arena, sizeof *si);
        si->cname = cname; si->init = init; si->type = vt; si->next = NULL;
        **tail = si;
        *tail = &si->next;
    } else {
        gen_initialiser(cg, vt, init);
    }
    emit(cg, ";\n");
}

void codegen_generate(CodeGen *cg, Node *prog) {
    cg->prog = prog;
    for (NodeList *it = prog->program.decls; it; it = it->next)
        if (it->node->kind == NK_CLASS_DECL) cg->uses_heap = true;   /* __new allocates */
    emit(cg, "/* Generated by Tamizhi " TAMIZHI_VERSION " */\n"
             "#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\n"
             "#include <stdint.h>\n#include <stddef.h>\n#include <setjmp.h>\n");
    for (NodeList *it = prog->program.decls; it; it = it->next)
        if (it->node->kind == NK_EXTERN_HEADER)
            emitf(cg, "#include %s\n", it->node->extern_header.spec);
    emit(cg, "\nint tz_argc;\nchar **tz_argv;\n\n");
    size_t rt_pos = cg->out.len;

    gen_enums(cg, prog);
    gen_types_ordered(cg, prog);
    gen_vtable_types(cg, prog);
    size_t list_pos = cg->out.len;

    bool any_proto = false;
    for (NodeList *it = prog->program.decls; it; it = it->next) {
        if (it->node->kind == NK_EXTERN_FUNC && !it->node->extern_func.has_header) {
            gen_extern_proto(cg, it->node);
            any_proto = true;
        }
    }
    if (any_proto) emit(cg, "\n");

    /* prototypes: functions, methods, constructors, allocators */
    for (NodeList *it = prog->program.decls; it; it = it->next) {
        Node *d = it->node;
        if (d->kind == NK_FUNC_DECL) {
            gen_func_header(cg, d);
            emit(cg, ";\n");
        } else if (d->kind == NK_CLASS_DECL) {
            for (Method *m = d->class_decl.methods; m; m = m->next) {
                if (m->is_abstract) continue;
                gen_method_header(cg, d, m);
                emit(cg, ";\n");
            }
            Method *ctor = d->class_decl.ctor;
            emitf(cg, "static tz_%s *tz_%s__new(%s);\n", d->class_decl.name,
                  d->class_decl.name,
                  params_str(cg, ctor ? ctor->params : NULL, false, NULL));
        }
    }
    emit(cg, "\n");

    /* globals and static fields */
    StartupInit *head = NULL, **tail = &head;
    for (NodeList *it = prog->program.decls; it; it = it->next) {
        Node *d = it->node;
        if (d->kind == NK_VAR_DECL) {
            TypeInfo *vt = d->var_decl.var_type ? d->var_decl.var_type : d->type;
            if (d->type && d->type->kind == TY_ARRAY) vt = d->type;
            gen_global(cg, vt, ident_str(cg, d->var_decl.name), d->var_decl.init,
                       d->var_decl.quals, &tail);
        } else if (d->kind == NK_CLASS_DECL) {
            for (Field *f = d->class_decl.fields; f; f = f->next)
                if (f->is_static)
                    gen_global(cg, f->type,
                               tfmt(cg, "tz_%s__%s", d->class_decl.name, f->name),
                               f->init, 0, &tail);
        }
    }
    gen_vtables(cg, prog);

    for (NodeList *it = prog->program.decls; it; it = it->next) {
        Node *d = it->node;
        if (d->kind == NK_FUNC_DECL) {
            cg->ret_type = d->func_decl.ret_type ? d->func_decl.ret_type : &ty_void;
            cg->try_depth = cg->break_base = cg->cont_base = 0;
            emit(cg, "\n");
            emit_line(cg, d);
            gen_func_header(cg, d);
            emit(cg, " ");
            if (!strcmp(d->func_decl.name, "main"))
                emit(cg, "{ tz_argc = argc; tz_argv = argv;\n");
            gen_block(cg, d->func_decl.body);
            if (!strcmp(d->func_decl.name, "main")) emit(cg, " }");
            emit(cg, "\n");
        } else if (d->kind == NK_CLASS_DECL) {
            for (Method *m = d->class_decl.methods; m; m = m->next)
                if (!m->is_abstract) gen_method_body(cg, d, m);
            gen_new_function(cg, d);
        }
    }

    if (head) {
        emit(cg, "\n/* globals whose initialiser is not a C constant */\n"
                 "static void tz_init_globals(void) __attribute__((constructor(102)));\n"
                 "static void tz_init_globals(void) {\n");
        for (StartupInit *si = head; si; si = si->next) {
            emitf(cg, "    %s = ", si->cname);
            gen_conv(cg, si->type, si->init);
            emit(cg, ";\n");
        }
        emit(cg, "}\n");
    }

    StrBuf rt;
    strbuf_init(&rt);
    if (cg->uses_checks || cg->uses_throw) {
        strbuf_append(&rt, "#define TZ_FILE ");
        StrBuf q; strbuf_init(&q);
        const char *f = cg->filename ? cg->filename : "?";
        strbuf_append(&q, "\"");
        for (const char *p = f; *p; p++) {
            if (*p == '"' || *p == '\\') strbuf_append_n(&q, "\\", 1);
            if ((unsigned char)*p >= 0x20) strbuf_append_n(&q, p, 1);
        }
        strbuf_append(&q, "\"");
        strbuf_append(&rt, q.data);
        strbuf_free(&q);
        strbuf_append(&rt, "\n");
        strbuf_append(&rt, RT_THROW);
        if (cg->uses_checks) strbuf_append(&rt, RT_CHECKS);
    }
    if (cg->uses_heap) {
        strbuf_append(&rt, cg->gc ? RT_GC : RT_MALLOC);
        strbuf_append(&rt, "\n");
        strbuf_append(&rt, RT_COMMON);
        strbuf_append(&rt, RT_GROW);
    }
    if (cg->nlists || cg->nmaps) {
        insert_at(cg, list_pos, cg->lists.data, "");
        StrBuf fw; strbuf_init(&fw);
        for (int i = 0; i < cg->nlists; i++)
            strbuf_appendf(&fw, "typedef struct tz_List_%d tz_List_%d;\n", i, i);
        for (int i = 0; i < cg->nmaps; i++)
            strbuf_appendf(&fw, "typedef struct tz_Map_%d tz_Map_%d;\n", i, i);
        strbuf_append(&fw, "\n");
        insert_at(cg, rt_pos, fw.data, "");
        strbuf_free(&fw);
        list_pos = 0; (void)list_pos;
    }
    if (rt.len) insert_at(cg, rt_pos, rt.data, "");
    strbuf_free(&rt);
}

void codegen_write(CodeGen *cg, FILE *fp) {
    fwrite(cg->out.data, 1, cg->out.len, fp);
}

void codegen_free(CodeGen *cg) { strbuf_free(&cg->out); strbuf_free(&cg->lists); }
