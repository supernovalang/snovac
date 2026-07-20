/* lex_internal.h — shared lexer state.
 *
 * Split by concern to keep every translation unit small:
 *   lex.c          driver: whitespace, comments, identifiers, operators
 *   lex_token.c    keyword table and token spelling
 *   lex_literal.c  string, char and number literals
 */
#ifndef SNOVAC_LEX_INTERNAL_H
#define SNOVAC_LEX_INTERNAL_H

#include "lex.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    uint32_t line;
    uint32_t line_start; /* byte offset where the current line begins */
    SnArena *arena;
    SnDiagSink *diag;
    SnTokenVec *out;
    int had_error;
} Lexer;

/* ── cursor ───────────────────────────────────────────────────────────────── */

static inline int at_end(const Lexer *L) { return L->pos >= L->len; }

static inline char peek(const Lexer *L) {
    return at_end(L) ? '\0' : L->src[L->pos];
}

static inline char peek2(const Lexer *L) {
    return L->pos + 1 >= L->len ? '\0' : L->src[L->pos + 1];
}

static inline char advance(Lexer *L) {
    char c = L->src[L->pos++];
    if (c == '\n') {
        L->line++;
        L->line_start = (uint32_t)L->pos;
    }
    return c;
}

static inline SnSpan span_from(const Lexer *L, size_t start, uint32_t start_line,
                               uint32_t start_line_begin) {
    SnSpan s;
    s.offset = (uint32_t)start;
    s.len = (uint32_t)(L->pos - start);
    s.line = start_line;
    s.col = (uint32_t)(start - start_line_begin) + 1u;
    return s;
}

/* ── character classes ────────────────────────────────────────────────────── */

static inline int is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c >= 0x80; /* UTF-8 continuation: identifiers may be non-ASCII */
}

static inline int is_ident_cont(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static inline int is_digit(char c) { return c >= '0' && c <= '9'; }

static inline int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* ── lex.c ────────────────────────────────────────────────────────────────── */

void vec_push(Lexer *L, SnToken t);
void emit(Lexer *L, SnTokKind kind, size_t start, uint32_t sl, uint32_t slb);

/* ── lex_token.c ──────────────────────────────────────────────────────────── */

SnTokKind keyword_lookup(const char *s, size_t n);
const char *keyword_spelling(SnTokKind k); /* NULL when k is not a keyword */

/* ── lex_literal.c ────────────────────────────────────────────────────────── */

void scan_string(Lexer *L, size_t start, uint32_t sl, uint32_t slb);
void scan_char(Lexer *L, size_t start, uint32_t sl, uint32_t slb);
void scan_number(Lexer *L, size_t start, uint32_t sl, uint32_t slb);

#endif /* SNOVAC_LEX_INTERNAL_H */
