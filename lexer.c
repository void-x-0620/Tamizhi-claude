/*
 * lexer.c — Single-pass hand-written lexer.
 *
 *  • One linear scan, one pointer; no regex, no backtracking.
 *  • Tokens point into the source buffer (zero copies for identifiers).
 *  • Only string literals allocate (exactly sized, from the arena).
 *  • Keywords are matched by a first-character switch + memcmp.
 *  • '#' starts a comment that runs to end of line.
 */
#include "lexer.h"
#include <errno.h>
#include <limits.h>

/* ─── Keyword recognition ──────────────────────────────────────────────── */
static bool kw_eq(const char *s, int len, const char *kw) {
    return (int)strlen(kw) == len && memcmp(s, kw, (size_t)len) == 0;
}

static const struct { const char *word; TokenType type; } KEYWORDS[] = {
    { "aga",        TOK_AGA        }, { "edu",    TOK_EDU    },
    { "illana",     TOK_ILLANA     }, { "irundha", TOK_IRUNDHA },
    { "kattamaipu", TOK_KATTAMAIPU }, { "kudu",   TOK_KUDU   },
    { "pathi",      TOK_PATHI      }, { "seyal",  TOK_SEYAL  },
    { "serku",      TOK_SERKU      }, { "sutri",  TOK_SUTRI  },
    { "thodar",     TOK_THODAR     }, { "udai",   TOK_UDAI   },
    { "veli",       TOK_VELI       },
    { "nilai",      TOK_NILAI      }, { "thangu",     TOK_THANGU     },
    { "maaridum",   TOK_MAARIDUM   }, { "ovvoru",     TOK_OVVORU     },
    { "seithu",     TOK_SEITHU     }, { "thervu",     TOK_THERVU     },
    { "vazhakku",   TOK_VAZHAKKU   }, { "iyalbu",     TOK_IYALBU     },
    { "pattiyal",   TOK_PATTIYAL   }, { "inaippu",    TOK_INAIPPU    },
    { "peyar",      TOK_PEYAR      }, { "vagupu",     TOK_VAGUPU     },
    { "uruvaakku",  TOK_URUVAAKKU  }, { "ithu",       TOK_ITHU       },
    { "thandhai",   TOK_THANDHAI   }, { "puthu",      TOK_PUTHU      },
    { "maraivu",    TOK_MARAIVU    }, { "paathukaappu", TOK_PAATHUKAAPPU },
    { "pothu",      TOK_POTHU      }, { "illai",      TOK_ILLAI      },
    { "maatru",     TOK_MAATRU     }, { "alavu",      TOK_ALAVU      },
    { "irudhi",     TOK_IRUDHI     }, { "muyarchi",   TOK_MUYARCHI   }, { "pidi",       TOK_PIDI       },
    { "erivi",      TOK_ERIVI      }, { "ulle",       TOK_ULLE       }, { "moolam",     TOK_MOOLAM     }, { "idaimugam",  TOK_IDAIMUGAM  },
};

static TokenType keyword_lookup(const char *s, int len) {
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        const char *w = KEYWORDS[i].word;
        if (w[0] == s[0] && kw_eq(s, len, w)) return KEYWORDS[i].type;
    }
    return TOK_IDENT;
}

/* ─── Character helpers ────────────────────────────────────────────────── */
static inline bool at_end(Lexer *L)   { return *L->current == '\0'; }
static inline char peek_ch(Lexer *L)  { return *L->current; }
static inline char peek_ch2(Lexer *L) { return L->current[0] ? L->current[1] : '\0'; }

static char advance_ch(Lexer *L) {
    char c = *L->current++;
    if (c == '\n') { L->line++; L->col = 1; } else { L->col++; }
    return c;
}

static void skip_trivia(Lexer *L) {
    for (;;) {
        char c = peek_ch(L);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_ch(L);
        } else if (c == '#') {
            while (!at_end(L) && peek_ch(L) != '\n') advance_ch(L);
        } else {
            break;
        }
    }
}

static Token make_token(Lexer *L, TokenType type, const char *start,
                        int line, int col) {
    Token t;
    memset(&t, 0, sizeof t);
    t.type   = type;
    t.start  = start;
    t.length = (int)(L->current - start);
    t.line   = line;
    t.col    = col;
    return t;
}

/* String literal. Pass 1 finds the closing quote so the unescaped result can
 * be allocated at its exact maximum size; pass 2 decodes the escapes. */
static Token lex_string(Lexer *L, const char *start, int line, int col) {
    const char *p = L->current;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    if (*p != '"') {
        report_error(L->filename, line, col, "unterminated string literal");
        while (!at_end(L)) advance_ch(L);
        return make_token(L, TOK_ERROR, start, line, col);
    }

    char *buf = arena_alloc(L->arena, (size_t)(p - L->current) + 1);
    size_t n = 0;
    while (peek_ch(L) != '"') {
        int ecol = L->col;
        char c = advance_ch(L);
        if (c == '\\') {
            char e = advance_ch(L);
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                default:
                    report_error(L->filename, L->line, ecol,
                                 "unknown escape sequence '\\%c'", e);
                    c = e;
                    break;
            }
        }
        buf[n++] = c;
    }
    advance_ch(L);                                   /* closing quote */
    buf[n] = '\0';

    Token t = make_token(L, TOK_STRING_LIT, start, line, col);
    t.val.str_val = buf;
    return t;
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

/* Decimal, 0xHEX, 0b101, 0o17, and floats with optional exponent. */
static Token lex_number(Lexer *L, const char *start, int line, int col) {
    int base = 10;
    if (*start == '0') {
        char p = peek_ch(L);
        if (p == 'x' || p == 'X') base = 16;
        else if (p == 'b' || p == 'B') base = 2;
        else if (p == 'o' || p == 'O') base = 8;
    }

    if (base != 10) {
        advance_ch(L);                                    /* the x / b / o */
        const char *digits = L->current;
        while (digit_value(peek_ch(L)) < base) advance_ch(L);
        if (L->current == digits) {
            report_error(L->filename, line, col,
                         "a base-%d literal needs at least one digit", base);
            return make_token(L, TOK_ERROR, start, line, col);
        }
        Token t = make_token(L, TOK_INT_LIT, start, line, col);
        errno = 0;
        unsigned long long v = strtoull(digits, NULL, base);
        if (errno == ERANGE) {
            report_error(L->filename, line, col, "integer literal is too large");
            t.type = TOK_ERROR;
        }
        t.val.int_val = (long long)v;
        t.uns = v > (unsigned long long)LLONG_MAX;
        return t;
    }

    while (isdigit((unsigned char)peek_ch(L))) advance_ch(L);
    bool is_float = false;
    if (peek_ch(L) == '.' && isdigit((unsigned char)peek_ch2(L))) {
        is_float = true;
        advance_ch(L);
        while (isdigit((unsigned char)peek_ch(L))) advance_ch(L);
    }
    if (peek_ch(L) == 'e' || peek_ch(L) == 'E') {          /* exponent */
        const char *q = L->current + 1;
        if (*q == '+' || *q == '-') q++;
        if (isdigit((unsigned char)*q)) {
            is_float = true;
            while (L->current < q) advance_ch(L);
            while (isdigit((unsigned char)peek_ch(L))) advance_ch(L);
        }
    }
    Token t = make_token(L, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT,
                         start, line, col);
    if (is_float) {
        t.val.float_val = strtod(start, NULL);
    } else {
        errno = 0;
        unsigned long long v = strtoull(start, NULL, 10);
        if (errno == ERANGE) {
            report_error(L->filename, line, col, "integer literal is too large");
            t.type = TOK_ERROR;
        }
        t.val.int_val = (long long)v;
        t.uns = v > (unsigned long long)LLONG_MAX;
    }
    return t;
}

/* 'a', '\n', '\x41', '\0' — a single byte, type char. */
static Token lex_char(Lexer *L, const char *start, int line, int col) {
    int value = 0;
    char c = peek_ch(L);
    if (at_end(L) || c == '\n' || c == '\'') {
        report_error(L->filename, line, col, "empty or unterminated character literal");
        return make_token(L, TOK_ERROR, start, line, col);
    }
    advance_ch(L);
    if (c == '\\') {
        char e = advance_ch(L);
        switch (e) {
            case 'n': value = '\n'; break;   case 't': value = '\t'; break;
            case 'r': value = '\r'; break;   case '0': value = 0;    break;
            case '\\': value = '\\'; break;  case '\'': value = '\''; break;
            case '"': value = '"'; break;
            case 'x': {
                int d1 = digit_value(peek_ch(L));
                if (d1 > 15) {
                    report_error(L->filename, line, col, "\\x needs hex digits");
                    return make_token(L, TOK_ERROR, start, line, col);
                }
                advance_ch(L);
                value = d1;
                int d2 = digit_value(peek_ch(L));
                if (d2 <= 15) { advance_ch(L); value = value * 16 + d2; }
                break;
            }
            default:
                report_error(L->filename, line, col,
                             "unknown escape sequence '\\%c'", e);
                break;
        }
    } else {
        value = (unsigned char)c;
        if (value >= 0x80) {
            report_error(L->filename, line, col,
                         "a character literal holds one byte; use a string for "
                         "non-ASCII text");
            while (!at_end(L) && peek_ch(L) != '\'' && peek_ch(L) != '\n') advance_ch(L);
            if (peek_ch(L) == '\'') advance_ch(L);
            return make_token(L, TOK_ERROR, start, line, col);
        }
    }
    if (peek_ch(L) != '\'') {
        report_error(L->filename, line, col, "unterminated character literal");
        return make_token(L, TOK_ERROR, start, line, col);
    }
    advance_ch(L);
    Token t = make_token(L, TOK_CHAR_LIT, start, line, col);
    t.val.int_val = value;
    return t;
}

/* ─── Public API ───────────────────────────────────────────────────────── */
void lexer_init(Lexer *L, const char *source, const char *filename,
                Arena *arena) {
    L->source = L->current = source;
    L->line = 1;
    L->col = 1;
    L->filename = filename;
    L->has_peeked = false;
    L->arena = arena;
}

Token lexer_next(Lexer *L) {
    if (L->has_peeked) {
        L->has_peeked = false;
        return L->peeked;
    }

    skip_trivia(L);
    const char *start = L->current;
    int line = L->line, col = L->col;
    if (at_end(L)) return make_token(L, TOK_EOF, start, line, col);

    char c = advance_ch(L);

    if (isdigit((unsigned char)c)) return lex_number(L, start, line, col);
    if (c == '"')                  return lex_string(L, start, line, col);
    if (c == '$' && peek_ch(L) == '"') {           /* $"text {expr}": interpolated string */
        advance_ch(L);
        Token t = lex_string(L, start, line, col);
        if (t.type == TOK_STRING_LIT) t.type = TOK_ISTRING_LIT;
        return t;
    }
    if (c == '\'')                 return lex_char(L, start, line, col);

    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)peek_ch(L)) || peek_ch(L) == '_')
            advance_ch(L);
        int len = (int)(L->current - start);
        return make_token(L, keyword_lookup(start, len), start, line, col);
    }

#define TOK(t) return make_token(L, (t), start, line, col)
#define TWO(next, yes, no) \
    if (peek_ch(L) == (next)) { advance_ch(L); TOK(yes); } TOK(no)

    switch (c) {
        case '(': TOK(TOK_LPAREN);    case ')': TOK(TOK_RPAREN);
        case '{': TOK(TOK_LBRACE);    case '}': TOK(TOK_RBRACE);
        case '[': TOK(TOK_LBRACKET);  case ']': TOK(TOK_RBRACKET);
        case ';': TOK(TOK_SEMICOLON); case ',': TOK(TOK_COMMA);
        case '.': TWO('.', TOK_DOTDOT, TOK_DOT);   case ':': TOK(TOK_COLON);
        case '@': TOK(TOK_AT);
        case '~': TOK(TOK_TILDE);     case '?': TOK(TOK_QUESTION);
        case '+': if (peek_ch(L) == '+') { advance_ch(L); TOK(TOK_PLUS_PLUS); }
                  TWO('=', TOK_PLUS_EQ, TOK_PLUS);
        case '-': if (peek_ch(L) == '-') { advance_ch(L); TOK(TOK_MINUS_MINUS); }
                  TWO('=', TOK_MINUS_EQ, TOK_MINUS);
        case '*': TWO('=', TOK_STAR_EQ,    TOK_STAR);
        case '/': TWO('=', TOK_SLASH_EQ,   TOK_SLASH);
        case '%': TWO('=', TOK_PERCENT_EQ, TOK_PERCENT);
        case '^': TWO('=', TOK_CARET_EQ,   TOK_CARET);
        case '!': TWO('=', TOK_BANG_EQ, TOK_BANG);
        case '=': TWO('=', TOK_EQ_EQ,   TOK_EQ);
        case '<': if (peek_ch(L) == '<') { advance_ch(L); TWO('=', TOK_SHL_EQ, TOK_SHL); }
                  TWO('=', TOK_LT_EQ, TOK_LT);
        case '>': if (peek_ch(L) == '>') { advance_ch(L); TWO('=', TOK_SHR_EQ, TOK_SHR); }
                  TWO('=', TOK_GT_EQ, TOK_GT);
        case '&': if (peek_ch(L) == '&') { advance_ch(L); TOK(TOK_AMP_AMP); }
                  TWO('=', TOK_AMP_EQ, TOK_AMP);
        case '|': if (peek_ch(L) == '|') { advance_ch(L); TOK(TOK_PIPE_PIPE); }
                  TWO('=', TOK_PIPE_EQ, TOK_PIPE);
        default: break;
    }
#undef TWO
#undef TOK

    if (isprint((unsigned char)c))
        report_error(L->filename, line, col, "unexpected character '%c'", c);
    else
        report_error(L->filename, line, col,
                     "unexpected byte 0x%02x", (unsigned char)c);
    return make_token(L, TOK_ERROR, start, line, col);
}

Token lexer_peek(Lexer *L) {
    if (!L->has_peeked) {
        L->peeked = lexer_next(L);
        L->has_peeked = true;
    }
    return L->peeked;
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_INT_LIT:    return "integer literal";
        case TOK_FLOAT_LIT:  return "float literal";
        case TOK_STRING_LIT: return "string literal";
        case TOK_ISTRING_LIT: return "interpolated string";
        case TOK_IDENT:      return "identifier";
        case TOK_LPAREN:     return "'('";
        case TOK_RPAREN:     return "')'";
        case TOK_LBRACE:     return "'{'";
        case TOK_RBRACE:     return "'}'";
        case TOK_LBRACKET:   return "'['";
        case TOK_RBRACKET:   return "']'";
        case TOK_SEMICOLON:  return "';'";
        case TOK_COMMA:      return "','";
        case TOK_DOT:        return "'.'";
        case TOK_DOTDOT:     return "'..'";
        case TOK_COLON:      return "':'";
        case TOK_AT:         return "'@'";
        case TOK_CARET:      return "'^'";
        case TOK_PLUS:       return "'+'";
        case TOK_MINUS:      return "'-'";
        case TOK_STAR:       return "'*'";
        case TOK_SLASH:      return "'/'";
        case TOK_PERCENT:    return "'%'";
        case TOK_BANG:       return "'!'";
        case TOK_BANG_EQ:    return "'!='";
        case TOK_EQ:         return "'='";
        case TOK_EQ_EQ:      return "'=='";
        case TOK_LT:         return "'<'";
        case TOK_LT_EQ:      return "'<='";
        case TOK_GT:         return "'>'";
        case TOK_GT_EQ:      return "'>='";
        case TOK_AMP_AMP:    return "'&&'";
        case TOK_PIPE_PIPE:  return "'||'";
        case TOK_EDU:        return "'edu'";
        case TOK_SEYAL:      return "'seyal'";
        case TOK_AGA:        return "'aga'";
        case TOK_IRUNDHA:    return "'irundha'";
        case TOK_ILLANA:     return "'illana'";
        case TOK_SUTRI:      return "'sutri'";
        case TOK_UDAI:       return "'udai'";
        case TOK_THODAR:     return "'thodar'";
        case TOK_KUDU:       return "'kudu'";
        case TOK_PATHI:      return "'pathi'";
        case TOK_KATTAMAIPU: return "'kattamaipu'";
        case TOK_SERKU:      return "'serku'";
        case TOK_VELI:       return "'veli'";
        case TOK_CHAR_LIT: return "character literal";
        case TOK_AMP: return "'&'";
        case TOK_PIPE: return "'|'";
        case TOK_TILDE: return "'~'";
        case TOK_SHL: return "'<<'";
        case TOK_SHR: return "'>>'";
        case TOK_QUESTION: return "'?'";
        case TOK_PLUS_EQ: return "'+='";
        case TOK_MINUS_EQ: return "'-='";
        case TOK_STAR_EQ: return "'*='";
        case TOK_SLASH_EQ: return "'/='";
        case TOK_PERCENT_EQ: return "'%='";
        case TOK_AMP_EQ: return "'&='";
        case TOK_PIPE_EQ: return "'|='";
        case TOK_CARET_EQ: return "'^='";
        case TOK_SHL_EQ: return "'<<='";
        case TOK_SHR_EQ: return "'>>='";
        case TOK_PLUS_PLUS: return "'++'";
        case TOK_MINUS_MINUS: return "'--'";
        case TOK_NILAI: return "'nilai'";
        case TOK_THANGU: return "'thangu'";
        case TOK_MAARIDUM: return "'maaridum'";
        case TOK_OVVORU: return "'ovvoru'";
        case TOK_SEITHU: return "'seithu'";
        case TOK_THERVU: return "'thervu'";
        case TOK_VAZHAKKU: return "'vazhakku'";
        case TOK_IYALBU: return "'iyalbu'";
        case TOK_PATTIYAL: return "'pattiyal'";
        case TOK_INAIPPU: return "'inaippu'";
        case TOK_PEYAR: return "'peyar'";
        case TOK_VAGUPU: return "'vagupu'";
        case TOK_URUVAAKKU: return "'uruvaakku'";
        case TOK_ITHU: return "'ithu'";
        case TOK_THANDHAI: return "'thandhai'";
        case TOK_PUTHU: return "'puthu'";
        case TOK_MARAIVU: return "'maraivu'";
        case TOK_PAATHUKAAPPU: return "'paathukaappu'";
        case TOK_POTHU: return "'pothu'";
        case TOK_ILLAI: return "'illai'";
        case TOK_MUYARCHI: return "'muyarchi'";
        case TOK_PIDI: return "'pidi'";
        case TOK_IRUDHI: return "'irudhi'";
        case TOK_ERIVI: return "'erivi'";
        case TOK_ULLE: return "'ulle'";
        case TOK_MOOLAM: return "'moolam'";
        case TOK_IDAIMUGAM: return "'idaimugam'";
        case TOK_MAATRU: return "'maatru'";
        case TOK_ALAVU: return "'alavu'";
        case TOK_EOF:        return "end of file";
        case TOK_ERROR:      return "invalid token";
    }
    return "<unknown>";
}
