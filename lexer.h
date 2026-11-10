/*
 * lexer.h — Tokens and lexer interface for Tamizhi.
 *
 * Keywords (Tanglish):
 *   edu seyal aga irundha illana sutri udai thodar kudu pathi kattamaipu
 * Conditionals are written  @(cond) aga irundha { }  ^(cond) aga irundha { }
 * illana { }   where '@' opens an if and '^' an else-if.
 */
#ifndef LEXER_H
#define LEXER_H

#include "tamizhi.h"

typedef enum {
    TOK_INT_LIT, TOK_CHAR_LIT, TOK_FLOAT_LIT, TOK_STRING_LIT, TOK_IDENT,

    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET, TOK_SEMICOLON, TOK_COMMA,
    TOK_DOT, TOK_DOTDOT, TOK_COLON, TOK_AT, TOK_CARET,

    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,

    TOK_BANG, TOK_BANG_EQ, TOK_EQ, TOK_EQ_EQ,
    TOK_LT, TOK_LT_EQ, TOK_GT, TOK_GT_EQ,
    TOK_AMP_AMP, TOK_PIPE_PIPE,
    TOK_AMP, TOK_PIPE, TOK_TILDE, TOK_SHL, TOK_SHR, TOK_QUESTION,
    TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ, TOK_PERCENT_EQ,
    TOK_AMP_EQ, TOK_PIPE_EQ, TOK_CARET_EQ, TOK_SHL_EQ, TOK_SHR_EQ,
    TOK_PLUS_PLUS, TOK_MINUS_MINUS,

    TOK_EDU, TOK_SEYAL, TOK_AGA, TOK_IRUNDHA, TOK_ILLANA, TOK_SUTRI,
    TOK_UDAI, TOK_THODAR, TOK_KUDU, TOK_PATHI, TOK_KATTAMAIPU,
    TOK_SERKU, TOK_VELI,
    /* qualifiers */
    TOK_NILAI, TOK_THANGU, TOK_MAARIDUM,
    /* more statements */
    TOK_OVVORU, TOK_SEITHU, TOK_THERVU, TOK_VAZHAKKU, TOK_IYALBU,
    /* type declarations */
    TOK_PATTIYAL, TOK_INAIPPU, TOK_PEYAR,
    /* object orientation */
    TOK_VAGUPU, TOK_URUVAAKKU, TOK_ITHU, TOK_THANDHAI, TOK_PUTHU,
    TOK_MARAIVU, TOK_PAATHUKAAPPU, TOK_POTHU,
    /* literals and operators spelled as words */
    TOK_ILLAI, TOK_MAATRU, TOK_ALAVU,
    TOK_MUYARCHI, TOK_PIDI, TOK_ERIVI, TOK_ULLE, TOK_MOOLAM,        /* try / catch / throw */

    TOK_EOF, TOK_ERROR,
} TokenType;

typedef struct {
    TokenType   type;
    const char *start;    /* points into the source buffer (not terminated) */
    int         length;
    int         line;
    int         col;
    bool        uns;      /* TOK_INT_LIT: value only fits as unsigned 64-bit */
    union {
        long long   int_val;
        double      float_val;
        const char *str_val;      /* arena-owned, NUL-terminated, unescaped */
    } val;
} Token;

typedef struct {
    const char *source;
    const char *current;
    int         line;
    int         col;
    const char *filename;
    Token       peeked;
    bool        has_peeked;
    Arena      *arena;
} Lexer;

void        lexer_init(Lexer *L, const char *source, const char *filename,
                       Arena *arena);
Token       lexer_next(Lexer *L);
Token       lexer_peek(Lexer *L);
const char *token_type_name(TokenType t);

#endif /* LEXER_H */
