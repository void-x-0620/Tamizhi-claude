/*
 * ast.c — AST construction helpers and pathi format analysis.
 */
#include "ast.h"

TypeInfo ty_void = { .kind = TY_VOID, .array_size = -1, .tname = "void", .cname = "void", .bits = 0, .is_unsigned = false };
TypeInfo ty_string = { .kind = TY_STRING, .array_size = -1, .tname = "string", .cname = "const char *", .bits = 64, .is_unsigned = false };
TypeInfo ty_bool = { .kind = TY_BOOL, .array_size = -1, .tname = "bool", .cname = "int", .bits = 32, .is_unsigned = false };
TypeInfo ty_unknown = { .kind = TY_UNKNOWN, .array_size = -1, .tname = "?", .cname = "long long", .bits = 0, .is_unsigned = false };
TypeInfo ty_null = { .kind = TY_NULL, .array_size = -1, .tname = "illai", .cname = "void *", .bits = 64, .is_unsigned = false };
TypeInfo ty_int = { .kind = TY_INT, .array_size = -1, .tname = "int", .cname = "long long", .bits = 64, .is_unsigned = false };
TypeInfo ty_float = { .kind = TY_FLOAT, .array_size = -1, .tname = "float", .cname = "double", .bits = 64, .is_unsigned = false };
TypeInfo ty_char = { .kind = TY_INT, .array_size = -1, .tname = "char", .cname = "char", .bits = 8, .is_unsigned = false };
TypeInfo ty_i8 = { .kind = TY_INT, .array_size = -1, .tname = "i8", .cname = "int8_t", .bits = 8, .is_unsigned = false };
TypeInfo ty_i16 = { .kind = TY_INT, .array_size = -1, .tname = "i16", .cname = "int16_t", .bits = 16, .is_unsigned = false };
TypeInfo ty_i32 = { .kind = TY_INT, .array_size = -1, .tname = "i32", .cname = "int32_t", .bits = 32, .is_unsigned = false };
TypeInfo ty_i64 = { .kind = TY_INT, .array_size = -1, .tname = "i64", .cname = "int64_t", .bits = 64, .is_unsigned = false };
TypeInfo ty_u8 = { .kind = TY_INT, .array_size = -1, .tname = "u8", .cname = "uint8_t", .bits = 8, .is_unsigned = true };
TypeInfo ty_u16 = { .kind = TY_INT, .array_size = -1, .tname = "u16", .cname = "uint16_t", .bits = 16, .is_unsigned = true };
TypeInfo ty_u32 = { .kind = TY_INT, .array_size = -1, .tname = "u32", .cname = "uint32_t", .bits = 32, .is_unsigned = true };
TypeInfo ty_u64 = { .kind = TY_INT, .array_size = -1, .tname = "u64", .cname = "uint64_t", .bits = 64, .is_unsigned = true };
TypeInfo ty_uchar = { .kind = TY_INT, .array_size = -1, .tname = "uchar", .cname = "unsigned char", .bits = 8, .is_unsigned = true };
TypeInfo ty_short = { .kind = TY_INT, .array_size = -1, .tname = "short", .cname = "short", .bits = 16, .is_unsigned = false };
TypeInfo ty_ushort = { .kind = TY_INT, .array_size = -1, .tname = "ushort", .cname = "unsigned short", .bits = 16, .is_unsigned = true };
TypeInfo ty_uint = { .kind = TY_INT, .array_size = -1, .tname = "uint", .cname = "unsigned int", .bits = 32, .is_unsigned = true };
TypeInfo ty_long = { .kind = TY_INT, .array_size = -1, .tname = "long", .cname = "long", .bits = 64, .is_unsigned = false };
TypeInfo ty_ulong = { .kind = TY_INT, .array_size = -1, .tname = "ulong", .cname = "unsigned long", .bits = 64, .is_unsigned = true };
TypeInfo ty_usize = { .kind = TY_INT, .array_size = -1, .tname = "usize", .cname = "size_t", .bits = 64, .is_unsigned = true };
TypeInfo ty_isize = { .kind = TY_INT, .array_size = -1, .tname = "isize", .cname = "ptrdiff_t", .bits = 64, .is_unsigned = false };
TypeInfo ty_f32 = { .kind = TY_FLOAT, .array_size = -1, .tname = "f32", .cname = "float", .bits = 32, .is_unsigned = false };
TypeInfo ty_f64 = { .kind = TY_FLOAT, .array_size = -1, .tname = "f64", .cname = "double", .bits = 64, .is_unsigned = false };
TypeInfo ty_ldouble = { .kind = TY_FLOAT, .array_size = -1, .tname = "ldouble", .cname = "long double", .bits = 128, .is_unsigned = false };

static const struct { const char *name; TypeInfo *type; } BUILTIN_TYPES[] = {
    { "int", &ty_int },
    { "float", &ty_float },
    { "string", &ty_string },
    { "bool", &ty_bool },
    { "void", &ty_void },
    { "char", &ty_char },
    { "i8", &ty_i8 },
    { "i16", &ty_i16 },
    { "i32", &ty_i32 },
    { "i64", &ty_i64 },
    { "u8", &ty_u8 },
    { "u16", &ty_u16 },
    { "u32", &ty_u32 },
    { "u64", &ty_u64 },
    { "uchar", &ty_uchar },
    { "short", &ty_short },
    { "ushort", &ty_ushort },
    { "uint", &ty_uint },
    { "long", &ty_long },
    { "ulong", &ty_ulong },
    { "usize", &ty_usize },
    { "isize", &ty_isize },
    { "f32", &ty_f32 },
    { "f64", &ty_f64 },
    { "ldouble", &ty_ldouble },
};

TypeInfo *builtin_type(const char *name, int len) {
    for (size_t i = 0; i < sizeof BUILTIN_TYPES / sizeof BUILTIN_TYPES[0]; i++)
        if ((int)strlen(BUILTIN_TYPES[i].name) == len &&
            !memcmp(BUILTIN_TYPES[i].name, name, (size_t)len))
            return BUILTIN_TYPES[i].type;
    return NULL;
}

const char *ast_current_file = NULL;

Node *node_new(Arena *a, NodeKind kind, int line, int col) {
    Node *n = arena_alloc(a, sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->col  = col;
    n->file = ast_current_file;
    return n;
}

void nlb_push(Arena *a, NodeListBuilder *b, Node *n) {
    NodeList *it = arena_alloc(a, sizeof(NodeList));
    it->node = n;
    it->next = NULL;
    if (b->tail) b->tail->next = it; else b->head = it;
    b->tail = it;
}

Param *param_new(Arena *a, const char *name, TypeInfo *type) {
    Param *p = arena_alloc(a, sizeof(Param));
    p->name = name; p->type = type; p->next = NULL;
    return p;
}

Field *field_new(Arena *a, const char *name, TypeInfo *type) {
    Field *f = arena_alloc(a, sizeof(Field));
    memset(f, 0, sizeof *f);
    f->name = name; f->type = type; f->access = ACC_PUBLIC;
    return f;
}

TypeInfo *type_new(Arena *a, TypeKind kind) {
    TypeInfo *t = arena_alloc(a, sizeof(TypeInfo));
    t->kind = kind; t->elem_type = NULL; t->struct_name = NULL;
    t->array_size = -1;
    t->tname = t->cname = NULL; t->bits = 0; t->is_unsigned = false;
    t->fparams = NULL; t->fret = NULL; t->key_type = NULL; t->is_set = false;
    t->tup = NULL; t->ntup = 0;
    return t;
}

bool type_same(const TypeInfo *a, const TypeInfo *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_INT: case TY_FLOAT:
            return a->bits == b->bits && a->is_unsigned == b->is_unsigned &&
                   !strcmp(a->cname, b->cname);
        case TY_STRUCT: case TY_ENUM: case TY_CLASS:
            return !strcmp(a->struct_name, b->struct_name);
        case TY_POINTER: case TY_ARRAY: case TY_LIST:
            return type_same(a->elem_type, b->elem_type);
        case TY_MAP:
            return a->is_set == b->is_set && type_same(a->key_type, b->key_type) &&
                   type_same(a->elem_type, b->elem_type);
        case TY_FUNC: case TY_CLOSURE: {
            if (!type_same(a->fret, b->fret)) return false;
            const Param *x = a->fparams, *y = b->fparams;
            for (; x && y; x = x->next, y = y->next)
                if (!type_same(x->type, y->type)) return false;
            return !x && !y;
        }
        default: return true;
    }
}

TypeInfo *type_pointer(Arena *a, TypeInfo *elem) {
    TypeInfo *t = type_new(a, TY_POINTER);
    t->elem_type = elem;
    return t;
}

TypeInfo *type_func(Arena *a, struct Param *params, TypeInfo *ret) {
    TypeInfo *t = type_new(a, TY_FUNC);
    t->fparams = params;
    t->fret = ret ? ret : &ty_void;
    return t;
}

TypeInfo *type_array(Arena *a, TypeInfo *elem, int size) {
    TypeInfo *t = type_new(a, TY_ARRAY);
    t->elem_type = elem;
    t->array_size = size;
    return t;
}

const char *binop_name(BinOp op) {
    switch (op) {
        case BOP_ADD: return "+";  case BOP_SUB: return "-";
        case BOP_MUL: return "*";  case BOP_DIV: return "/";
        case BOP_MOD: return "%";  case BOP_EQ:  return "==";
        case BOP_NE:  return "!="; case BOP_LT:  return "<";
        case BOP_LE:  return "<="; case BOP_GT:  return ">";
        case BOP_GE:  return ">="; case BOP_AND: return "&&";
        case BOP_OR:  return "||"; case BOP_BAND: return "&";
        case BOP_BOR: return "|";  case BOP_BXOR: return "^";
        case BOP_SHL: return "<<"; case BOP_SHR: return ">>";
    }
    return "?";
}

const char *unaryop_name(UnaryOp op) {
    switch (op) {
        case UOP_NEG: return "-";  case UOP_NOT:   return "!";
        case UOP_BNOT: return "~"; case UOP_ADDR:  return "&";
        case UOP_DEREF: return "*";
    }
    return "?";
}

/* ─── Format strings ───────────────────────────────────────────────────── */
char *fmt_rewrite(Arena *a, const char *fmt, char *kinds, int max_kinds,
                  int *count, const char **err) {
    size_t n = strlen(fmt);
    char  *out = arena_alloc(a, n * 3 + 1);   /* a spec grows by at most 2x */
    size_t o = 0;
    int    k = 0;
    *err = NULL;

    for (const char *p = fmt; *p; ) {
        if (*p != '%') { out[o++] = *p++; continue; }
        out[o++] = *p++;
        if (*p == '%') { out[o++] = *p++; continue; }

        while (*p && strchr("-+ #0", *p)) out[o++] = *p++;     /* flags     */
        while (isdigit((unsigned char)*p)) out[o++] = *p++;    /* width     */
        if (*p == '.') {                                       /* precision */
            out[o++] = *p++;
            while (isdigit((unsigned char)*p)) out[o++] = *p++;
        }
        while (*p && strchr("hlLqjzt", *p)) p++;               /* dropped   */

        char c = *p;
        if (!c) { *err = "incomplete conversion at end of format string"; return NULL; }
        p++;

        char kind;
        switch (c) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
                kind = 'i'; out[o++] = 'l'; out[o++] = 'l'; out[o++] = c; break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
                kind = 'f'; out[o++] = c; break;
            case 's': kind = 's'; out[o++] = c; break;
            case 'c': kind = 'c'; out[o++] = c; break;
            default:
                *err = (c == 'n') ? "%n conversions are not allowed"
                                  : "unsupported conversion specifier";
                return NULL;
        }
        if (k >= max_kinds) { *err = "too many conversions in format string"; return NULL; }
        kinds[k++] = kind;
    }
    out[o] = '\0';
    *count = k;
    return out;
}
