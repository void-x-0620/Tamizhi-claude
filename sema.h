/*
 * sema.h — Semantic analysis.
 *
 * One pass over the AST (after hoisting all type and function declarations
 * so declaration order is free) that
 *   • builds a scoped symbol table (hash buckets per scope, arena-allocated),
 *   • resolves identifiers, struct/union/enum/class/typedef names and fields,
 *   • annotates every expression node with its type, using C's usual
 *     arithmetic conversions for the numeric family,
 *   • enforces nilai (const) and the narrowing rules (maatru is required to
 *     narrow), class access control, override signatures and constructor
 *     chaining,
 *   • reports type errors and a few memory-safety errors at compile time
 *     (constant out-of-bounds indices, division by constant zero, bad
 *     shift counts, mismatched pathi format strings).
 */
#ifndef SEMA_H
#define SEMA_H

#include "ast.h"

typedef enum { SYM_VAR, SYM_FUNC, SYM_STRUCT, SYM_ENUM, SYM_CLASS,
               SYM_TYPEDEF } SymKind;

typedef struct Symbol {
    const char    *name;
    SymKind        kind;
    TypeInfo      *type;     /* variable type / function return type / the type itself */
    Param         *params;   /* SYM_FUNC   */
    Field         *fields;   /* SYM_STRUCT */
    Enumerator    *enums;    /* SYM_ENUM   */
    Node          *decl;     /* SYM_STRUCT / SYM_CLASS / SYM_TYPEDEF */
    const char    *cname;    /* SYM_FUNC from a C binding, else NULL */
    bool           is_const; /* SYM_VAR declared nilai */
    bool           used, track;   /* unused-variable warning */
    int            line, col;
    int            state;    /* lazy resolution: 0 new, 1 busy, 2 done */
    struct Symbol *next;
} Symbol;

#define SCOPE_BUCKETS 64

typedef struct Scope {
    Symbol       *buckets[SCOPE_BUCKETS];
    struct Scope *parent;
} Scope;

typedef struct {
    Scope      *scope;
    Scope      *global;
    Arena      *arena;
    const char *filename;      /* file of the declaration being checked */
    const char *main_file;
    TypeInfo   *current_ret;   /* return type of the function being checked */
    bool        has_main;
    Node       *cur_class;     /* class whose method is being checked */
    Method     *cur_method;
    Node       *ctor_call;     /* the one legal thandhai(...) call, if any */
    int         loop_depth;    /* loops + switches, for diagnostics */
    void       *lam;           /* innermost lambda being checked (LambdaCtx) */
} Sema;

void sema_init(Sema *s, Arena *arena, const char *filename);
void sema_analyse(Sema *s, Node *program);

#endif /* SEMA_H */
