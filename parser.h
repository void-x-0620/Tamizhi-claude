/*
 * parser.h — Recursive-descent parser for Tamizhi.
 *
 *   program     ::= decl*
 *   decl        ::= func_decl | struct_decl | var_decl ';' | import | extern
 *   import      ::= 'serku' STRING ';'                 (include a binding file)
 *   extern      ::= 'veli' STRING ';'                  (C header to include)
 *                 | 'veli' 'seyal' IDENT ['=' IDENT]   (tamizhi name = C name)
 *                   '(' [IDENT ':' type (',' IDENT ':' type)*] ')' [':' type] ';'
 *   func_decl   ::= 'seyal' IDENT '(' [param (',' param)*] ')' [':' type] block
 *   param       ::= IDENT [':' type]
 *   struct_decl ::= 'kattamaipu' IDENT '{' (IDENT ':' type ';')* '}'
 *   var_decl    ::= 'edu' IDENT [':' type] ['=' expr]
 *   type        ::= IDENT ('[' [INT] ']')*        (int float string bool void
 *                                                  or a struct name)
 *   block       ::= '{' stmt* '}'
 *   stmt        ::= var_decl ';'
 *                 | '@' '(' expr ')' 'aga' 'irundha' block
 *                   ('^' '(' expr ')' 'aga' 'irundha' block)*
 *                   ['illana' block]
 *                 | 'sutri' '(' expr ')' block
 *                 | 'kudu' [expr] ';'  | 'udai' ';'  | 'thodar' ';'
 *                 | 'pathi' '(' expr (',' expr)* ')' ';'
 *                 | expr ';'
 *
 * Expression precedence, loosest to tightest:
 *   =  (right assoc)   ||   &&   == !=   < <= > >=   + -   * / %
 *   unary - !          postfix [] . ()          primary
 *
 * See SYNTAX.md for the complete 2.0 grammar.
 *
 * Error recovery: a syntax error sets `panic`; the enclosing statement or
 * declaration loop then skips to the next ';' / '}' / seyal / kattamaipu and
 * carries on, so several errors can be reported in one run.
 */
#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

/* Files already pulled in by 'serku' (each file is included at most once). */
#define MAX_IMPORTS 64
#define MAX_IMPORT_DEPTH 16
typedef struct {
    const char *paths[MAX_IMPORTS];
    int         count;
    int         depth;
} ImportSet;

typedef struct {
    Lexer  *lexer;
    Arena  *arena;
    bool    panic;
    int     loop_depth;
    int     func_depth;
    size_t  consumed;     /* tokens consumed so far; used to prove progress */
    int     switch_depth; /* inside thervu: udai is allowed here too */
    bool    binding;      /* parsing a binding file: only veli/serku allowed */
    bool    saw_header;   /* a 'veli "x.h"' came before: prototypes trusted */
    int nest;                        /* recursion depth guard */
    NodeList *lam_head, *lam_tail;   /* lifted lambdas, appended to the program */
    ImportSet *imports;   /* shared by the main file and everything it imports */
} Parser;

/* Extra directories searched by 'serku' after the including file's own (the
 * standard library lives in one of them). Call before parsing. */
void  parser_add_lib_dir(const char *dir);
void  parser_init(Parser *p, Lexer *L, Arena *arena);
Node *parse_program(Parser *p);

#endif /* PARSER_H */
