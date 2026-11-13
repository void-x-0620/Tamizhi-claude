/*
 * codegen.h — Tamizhi -> C99 back end.
 *
 * The compiler emits one self-contained C translation unit and lets the host
 * C compiler (cc/gcc/clang) do instruction selection, register allocation and
 * machine-level optimisation. That gives native-speed output with no
 * interpreter and no LLVM dependency.
 *
 * Optimisations done here, before C is emitted:
 *   • recursive constant folding (integer and float, overflow-checked)
 *   • safe peepholes: x+0, x-0, x*1, x/1, 0+x, 1*x, and x*0 for pure int x
 *   • short-circuit folding: false && x, true || x
 *   • dead-branch elimination for constant if / else-if / while conditions
 *   • unreachable-statement removal after kudu / udai / thodar
 *
 * Safety features (runtime checks can be turned off with --no-checks):
 *   • null-reference checks on object / pointer member access and deref
 *   • bounds checks on fixed-size arrays with a non-constant index
 *   • integer division / modulo by zero checks
 *   • every variable is zero-initialised
 *   • pathi formats are validated and normalised (%d -> %lld) and arguments
 *     are cast to the width the format expects
 *   • string == / < etc. lower to strcmp, not pointer comparison
 *   • user identifiers are prefixed with tz_ so they can never collide with
 *     C keywords or libc names
 */
#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

typedef struct {
    StrBuf      out;
    int         indent;
    Arena      *arena;
    const char *filename;
    bool        gc;          /* in:  use the Boehm collector for heap data     */
    bool        uses_heap;   /* out: the program allocates (needs -lgc if gc)  */
    bool        checks;      /* in:  emit null / bounds / divide-by-zero checks */
    bool        uses_checks; /* out: a check helper was emitted                */
    bool        lines;       /* in:  emit #line directives for debuggers      */
    bool        uses_throw;  /* out: try/catch/throw needs the frame runtime  */
    int         try_depth, break_base, cont_base;   /* active try frames     */
    StrBuf      lists;       /* C definitions of the List<T> instances used    */
    int         nlists, nmaps;
    const char *map_keys[256];
    const char *list_keys[256];
    Node       *prog;        /* the program being generated                    */
    TypeInfo   *ret_type;    /* return type of the function being generated    */
    Node       *cur_class;   /* class whose method is being generated          */
} CodeGen;

void codegen_init(CodeGen *cg, Arena *arena, const char *filename);
void codegen_generate(CodeGen *cg, Node *program);
void codegen_write(CodeGen *cg, FILE *fp);
void codegen_free(CodeGen *cg);

#endif /* CODEGEN_H */
