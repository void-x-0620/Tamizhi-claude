/*
 * ast.h — Abstract syntax tree for Tamizhi.
 *
 * Tagged-union nodes, all arena-allocated. Lists are singly linked and built
 * with a tail pointer (NodeListBuilder) so appends are O(1).
 */
#ifndef AST_H
#define AST_H

#include "tamizhi.h"

/* ─── Types ────────────────────────────────────────────────────────────── */
typedef enum {
    TY_VOID, TY_INT, TY_FLOAT, TY_STRING, TY_BOOL,
    TY_ARRAY, TY_STRUCT, TY_UNKNOWN,
    TY_POINTER,     /* T*                                              */
    TY_ENUM,        /* pattiyal                                         */
    TY_CLASS,       /* vagupu: a reference to a heap object             */
    TY_FUNC,        /* seyal(T, ...) : R   — pointer to a function      */
    TY_NULL,        /* the type of 'illai'                              */
    TY_CLOSURE,     /* Fn<(A,B): R>: function + captured environment (fparams/fret) */
    TY_MAP,         /* Map<K,V> / Set<T> (is_set): insertion-ordered hash table */
    TY_LIST,        /* List<T>: growable array, a heap reference         */
} TypeKind;

typedef struct TypeInfo TypeInfo;
struct TypeInfo {
    TypeKind    kind;
    TypeInfo   *elem_type;     /* TY_ARRAY, TY_POINTER, TY_LIST, TY_MAP value */
    TypeInfo   *key_type;      /* TY_MAP key (the element type of a Set)    */
    bool        is_set;        /* TY_MAP written as Set<T>                  */
    const char *struct_name;   /* TY_STRUCT/ENUM/CLASS name (or an alias)   */
    int         array_size;    /* TY_ARRAY: element count, -1 = unsized     */
    /* TY_INT / TY_FLOAT: every C arithmetic type is one of these kinds     */
    const char *tname;         /* spelling in Tamizhi, for messages         */
    const char *cname;         /* spelling in the generated C               */
    int         bits;
    bool        is_unsigned;
    /* TY_FUNC */
    struct Param *fparams;
    TypeInfo    *fret;
};

/* Shared singletons for the built-in types (never freed, never mutated).
 * int is a 64-bit long long and float is a double; the others are the exact
 * C types, so they can be passed to C code without conversion. */
extern TypeInfo ty_void, ty_int, ty_float, ty_string, ty_bool, ty_unknown,
                ty_null, ty_char, ty_i8, ty_i16, ty_i32, ty_i64, ty_u8, ty_u16,
                ty_u32, ty_u64, ty_uchar, ty_short, ty_ushort, ty_uint, ty_long,
                ty_ulong, ty_usize, ty_isize, ty_f32, ty_f64, ty_ldouble;
/* Structural type equality (struct/enum/class by name, arrays ignore size). */
bool type_same(const TypeInfo *a, const TypeInfo *b);
/* Look up a primitive type by its Tamizhi name (int, i32, uchar, f32 ...).
 * Returns NULL if `name` is not a primitive type. */
TypeInfo *builtin_type(const char *name, int len);

/* ─── Node kinds ───────────────────────────────────────────────────────── */
typedef enum {
    NK_PROGRAM, NK_FUNC_DECL, NK_VAR_DECL, NK_STRUCT_DECL, NK_BLOCK,
    NK_IF, NK_WHILE, NK_RETURN, NK_BREAK, NK_CONTINUE, NK_EXPR_STMT, NK_PATHI,

    NK_INT_LIT, NK_FLOAT_LIT, NK_STRING_LIT, NK_BOOL_LIT, NK_IDENT,
    NK_ASSIGN, NK_BINARY, NK_UNARY, NK_CALL, NK_INDEX, NK_MEMBER,
    NK_ARRAY_INIT,
    NK_EXTERN_FUNC, NK_EXTERN_HEADER,      /* C bindings (veli) */

    NK_FOR, NK_DO_WHILE, NK_SWITCH,
    NK_TERNARY, NK_CAST, NK_SIZEOF, NK_NULL_LIT, NK_INCDEC, NK_STRUCT_LIT,
    NK_ENUM_DECL, NK_TYPEDEF_DECL,
    NK_TRY, NK_THROW, NK_LIST_NEW, NK_FOREACH, NK_LAMBDA,

    /* object orientation */
    NK_CLASS_DECL, NK_NEW, NK_MCALL, NK_THIS, NK_SUPER, NK_CTOR_CALL,
} NodeKind;

typedef enum {
    BOP_ADD, BOP_SUB, BOP_MUL, BOP_DIV, BOP_MOD,
    BOP_EQ, BOP_NE, BOP_LT, BOP_LE, BOP_GT, BOP_GE,
    BOP_AND, BOP_OR,
    BOP_BAND, BOP_BOR, BOP_BXOR, BOP_SHL, BOP_SHR,
} BinOp;

#define BOP_NONE (-1)          /* plain '=' in an NK_ASSIGN */

typedef enum { UOP_NEG, UOP_NOT, UOP_BNOT, UOP_ADDR, UOP_DEREF } UnaryOp;

/* Declaration qualifiers (nilai / thangu / maaridum). */
enum { Q_CONST = 1, Q_STATIC = 2, Q_VOLATILE = 4 };

typedef enum { ACC_PUBLIC, ACC_PRIVATE, ACC_PROTECTED } Access;

typedef struct Node Node;
typedef struct NodeList { Node *node; struct NodeList *next; } NodeList;

typedef struct Param { const char *name; TypeInfo *type; struct Param *next; } Param;
/* A local variable a lambda uses from its enclosing function (captured by value). */
typedef struct Capture {
    const char *name; TypeInfo *type; void *sym; struct Capture *next;
} Capture;
/* A struct/union field, or a class field. */
typedef struct Field {
    const char *name;
    TypeInfo   *type;
    struct Field *next;
    /* class fields only */
    Access      access;
    bool        is_static;
    Node       *init;        /* optional initialiser (static fields)  */
    int         line, col;
} Field;

/* A class method or constructor. */
typedef struct Method {
    const char *name;
    Param      *params;
    TypeInfo   *ret_type;
    Node       *body;
    bool        is_static, is_ctor, is_abstract;
    Access      access;
    int         line, col;
    struct Method *next;
    /* filled by sema */
    int         slot;                  /* vtable slot, -1 if not virtual     */
    const char *owner;                 /* declaring class                    */
    struct Method *overrides;          /* the parent method it replaces      */
} Method;

typedef struct Enumerator {
    const char *name;
    Node       *value;                 /* NULL: previous + 1                 */
    long long   resolved;              /* filled by sema                     */
    bool        done;                  /* resolved is valid                  */
    int         line, col;
    struct Enumerator *next;
} Enumerator;

typedef struct FieldInit {
    const char *name;
    Node       *value;
    int         line, col;
    struct FieldInit *next;
} FieldInit;

/* One 'vazhakku a, b: { ... }' (or 'iyalbu: { ... }' when is_default). */
typedef struct SwitchCase {
    NodeList   *labels;                /* constant expressions                */
    bool        is_default;
    Node       *body;                  /* NK_BLOCK                            */
    int         line, col;
    struct SwitchCase *next;
} SwitchCase;

/* One arm of an if / else-if / else chain (cond == NULL for the else arm). */
typedef struct IfBranch {
    Node            *cond;
    Node            *body;
    struct IfBranch *next;
} IfBranch;

typedef struct { NodeList *head, *tail; } NodeListBuilder;

struct Node {
    NodeKind  kind;
    int       line, col;
    const char *file;                   /* source file (differs for headers) */
    bool      as_lvalue;                /* an index expression that is assigned to */
    bool      is_lambda;                /* a lifted seyal expression */
    TypeInfo *type;                     /* set by semantic analysis */

    union {
        struct { NodeList *decls; } program;
        struct { const char *name; Param *params; TypeInfo *ret_type;
                 Node *body; bool is_static; } func_decl;
        struct { const char *name; TypeInfo *var_type; Node *init;
                 int quals; } var_decl;          /* Q_CONST | Q_STATIC | Q_VOLATILE */
        struct { const char *name; Field *fields; bool is_union; } struct_decl;
        struct { NodeList *stmts; } block;
        struct { IfBranch *branches; } if_stmt;
        struct { Node *cond; Node *body; } while_stmt;
        struct { Node *value; } ret;
        struct { Node *expr; } expr_stmt;
        struct { Node *fmt; NodeList *args; } pathi;

        struct { long long value; bool is_unsigned, is_char; } int_lit;
        struct { double value; } float_lit;
        struct { const char *value; } string_lit;
        struct { bool value; } bool_lit;
        struct { const char *name; const char *cname; void *capsym; } ident;   /* cname: C binding used as a value */
        struct { Node *target; Node *value; int op; } assign;   /* op: BinOp or BOP_NONE */
        struct { BinOp op; Node *left; Node *right; } binary;
        struct { UnaryOp op; Node *operand; } unary;
        struct { const char *name; NodeList *args;
                 const char *cname;             /* set for C bindings */
                 struct Param *params;
                 void *capsym; bool is_closure; } call;  /* callee's parameters (sema) */
        struct { Node *array; Node *index; } index;
        struct { Node *fn; Capture *caps; int ncap; int id; } lambda;   /* seyal (...) { } expression */
        struct { Node *object; const char *field;
                 int hops;                 /* parent links to follow (inheritance) */
                 bool is_static;           /* static class field: a global */
                 const char *owner;        /* class that declares the field */
               } member;
        struct { NodeList *elements; } array_init;
        struct { Node *body; const char *catch_name; Node *catch_body; } try_stmt;
        struct { Node *value; } throw_stmt;
        struct { TypeInfo *ltype; NodeList *items; } list_new;   /* puthu List<T>(items) */
        struct { const char *name; TypeInfo *vtype; Node *coll; Node *body;
                 TypeInfo *elem; } foreach_stmt;                  /* ovvoru (edu x ulle c) */

        struct { Node *init, *cond, *update, *body; } for_stmt;
        struct { Node *subject; SwitchCase *cases; } switch_stmt;
        struct { Node *cond, *then_e, *else_e; } ternary;
        struct { TypeInfo *to; Node *expr; } cast;
        struct { TypeInfo *of_type; Node *expr; } size_of;   /* one is set */
        struct { Node *operand; bool is_inc, prefix; } incdec;
        struct { const char *name; FieldInit *inits; } struct_lit;
        struct { const char *name; Enumerator *items; } enum_decl;
        struct { const char *name; TypeInfo *target; } typedef_decl;

        /* vagupu Name [: Parent] { fields and methods } */
        struct { const char *name; const char *parent; bool is_abstract;
                 Field *fields; Method *methods;
                 /* filled by sema */
                 struct Node *parent_decl;
                 Method **vslots; int nvslots;
                 Method *ctor;
                 int state;                 /* 0 new, 1 preparing, 2 ready */
        } class_decl;
        struct { const char *name; NodeList *args; Method *ctor; } new_expr;
        struct { Node *object; const char *method; NodeList *args;
                 Method *resolved; bool is_static_call, is_super_call;
                 struct Param *fparams;
                 bool closure_field;
                 int builtin; } mcall;       /* builtin: List method id (0 = none) */   /* fparams: struct callback field */
        struct { NodeList *args; Method *ctor; } ctor_call;

        /* veli seyal name [= cname](params) [: type];  — a C function */
        struct { const char *name;     /* name used in Tamizhi code        */
                 const char *cname;    /* name of the C function           */
                 Param *params; TypeInfo *ret_type;
                 bool has_header;      /* a header declared its prototype  */
                 const char *file;     /* binding file, for diagnostics    */
        } extern_func;
        /* veli "header.h";  — spec is ready to paste after #include */
        struct { const char *spec; } extern_header;
    };
};

/* ─── Construction helpers ─────────────────────────────────────────────── */
/* File that node_new stamps on new nodes; the parser updates it per file. */
extern const char *ast_current_file;
Node     *node_new(Arena *a, NodeKind kind, int line, int col);
void      nlb_push(Arena *a, NodeListBuilder *b, Node *n);
Param    *param_new(Arena *a, const char *name, TypeInfo *type);
Field    *field_new(Arena *a, const char *name, TypeInfo *type);
TypeInfo *type_new(Arena *a, TypeKind kind);
TypeInfo *type_array(Arena *a, TypeInfo *elem, int size);
TypeInfo *type_pointer(Arena *a, TypeInfo *elem);
TypeInfo *type_func(Arena *a, struct Param *params, TypeInfo *ret);

const char *binop_name(BinOp op);
const char *unaryop_name(UnaryOp op);

/* ─── pathi format-string analysis ─────────────────────────────────────── */
/*
 * Validates a printf-style format and rewrites every conversion to the
 * width Tamizhi's runtime types use (%d -> %lld, %lf -> %f, ...).
 * kinds[] receives one of 'i' (integer), 'f' (float), 's' (string),
 * 'c' (character) per conversion. Returns the rewritten format (arena
 * allocated) or NULL with *err set. Rejects %n, %p, '*' widths, etc.
 */
char *fmt_rewrite(Arena *a, const char *fmt, char *kinds, int max_kinds,
                  int *count, const char **err);

#endif /* AST_H */
