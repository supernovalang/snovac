/* lex.c — lexer driver: whitespace, comments, identifiers and operators.
 * The keyword table lives in lex_token.c and literals in lex_literal.c. */
#include "lex_internal.h"

void vec_push(Lexer *L, SnToken t) {
    SnTokenVec *v = L->out;
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 256;
        SnToken *nd =
            (SnToken *)sn_arena_alloc(L->arena, ncap * sizeof(SnToken));
        if (v->data) {
            memcpy(nd, v->data, v->len * sizeof(SnToken));
        }
        v->data = nd;
        v->cap = ncap;
    }
    v->data[v->len++] = t;
}

void emit(Lexer *L, SnTokKind kind, size_t start, uint32_t sl, uint32_t slb) {
    SnToken t;
    t.kind = kind;
    t.span = span_from(L, start, sl, slb);
    t.text = sn_arena_strndup(L->arena, L->src + start, L->pos - start);
    t.has_interpolation = 0;
    vec_push(L, t);
}

/* Whitespace and comments, up to the next real token. */
static void skip_trivia(Lexer *L) {
    for (;;) {
        char c = peek(L);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(L);
            continue;
        }
        if (c == '/' && peek2(L) == '/') {
            while (!at_end(L) && peek(L) != '\n') {
                advance(L);
            }
            continue;
        }
        /* Block comments: the corpus contains zero of them, but accepting the
         * slash-star form costs nothing and keeps the lexer forward-compatible
         * with the grammar decision pending in plan.md §3. */
        if (c == '/' && peek2(L) == '*') {
            advance(L);
            advance(L);
            while (!at_end(L) && !(peek(L) == '*' && peek2(L) == '/')) {
                advance(L);
            }
            if (!at_end(L)) {
                advance(L);
                advance(L);
            }
            continue;
        }
        break;
    }
}

/* Punctuation and operators. The leading character is already consumed; `n1` is
 * the one after it. */
static void scan_operator(Lexer *L, char c, char n1, size_t start, uint32_t sl,
                          uint32_t slb) {
    switch (c) {
    case '(': emit(L, SN_TOK_LPAREN, start, sl, slb); return;
    case ')': emit(L, SN_TOK_RPAREN, start, sl, slb); return;
    case '{': emit(L, SN_TOK_LBRACE, start, sl, slb); return;
    case '}': emit(L, SN_TOK_RBRACE, start, sl, slb); return;
    case '[': emit(L, SN_TOK_LBRACKET, start, sl, slb); return;
    case ']': emit(L, SN_TOK_RBRACKET, start, sl, slb); return;
    case ',': emit(L, SN_TOK_COMMA, start, sl, slb); return;
    case '.': emit(L, SN_TOK_DOT, start, sl, slb); return;
    case ';': emit(L, SN_TOK_SEMI, start, sl, slb); return;
    case '@': emit(L, SN_TOK_AT, start, sl, slb); return;
    case '^': emit(L, SN_TOK_CARET, start, sl, slb); return;
    case '%': emit(L, SN_TOK_PERCENT, start, sl, slb); return;

    case ':':
        if (n1 == ':') { advance(L); emit(L, SN_TOK_COLONCOLON, start, sl, slb); }
        else { emit(L, SN_TOK_COLON, start, sl, slb); }
        return;
    case '?':
        if (n1 == '?') { advance(L); emit(L, SN_TOK_QQ, start, sl, slb); }
        else { emit(L, SN_TOK_QUESTION, start, sl, slb); }
        return;
    case '~':
        if (n1 == '>') { advance(L); emit(L, SN_TOK_TILDE_ARROW, start, sl, slb); }
        else { emit(L, SN_TOK_TILDE, start, sl, slb); }
        return;
    case '+':
        if (n1 == '=') { advance(L); emit(L, SN_TOK_PLUS_EQ, start, sl, slb); }
        else { emit(L, SN_TOK_PLUS, start, sl, slb); }
        return;
    case '-':
        if (n1 == '>') { advance(L); emit(L, SN_TOK_ARROW, start, sl, slb); }
        else if (n1 == '=') { advance(L); emit(L, SN_TOK_MINUS_EQ, start, sl, slb); }
        else { emit(L, SN_TOK_MINUS, start, sl, slb); }
        return;
    case '*':
        if (n1 == '=') { advance(L); emit(L, SN_TOK_STAR_EQ, start, sl, slb); }
        else { emit(L, SN_TOK_STAR, start, sl, slb); }
        return;
    case '/':
        if (n1 == '=') { advance(L); emit(L, SN_TOK_SLASH_EQ, start, sl, slb); }
        else { emit(L, SN_TOK_SLASH, start, sl, slb); }
        return;
    case '=':
        if (n1 == '=') { advance(L); emit(L, SN_TOK_EQ, start, sl, slb); }
        else if (n1 == '>') { advance(L); emit(L, SN_TOK_FATARROW, start, sl, slb); }
        else { emit(L, SN_TOK_ASSIGN, start, sl, slb); }
        return;
    case '!':
        if (n1 == '=') { advance(L); emit(L, SN_TOK_NE, start, sl, slb); }
        else { emit(L, SN_TOK_BANG, start, sl, slb); }
        return;
    case '<':
        /* No `<<`: Snovalang has no shift operators, and `<` is the generic
         * opener. Always one token. */
        if (n1 == '=') { advance(L); emit(L, SN_TOK_LE, start, sl, slb); }
        /* `<~` is the receive-bind operator (docs/spec-pulsar-defer-selfhost.md
         * §1). Unambiguous against the generic opener: no type starts with
         * `~`, so `<` immediately followed by `~` is never `List<~T>`. */
        else if (n1 == '~') { advance(L); emit(L, SN_TOK_RECV_BIND, start, sl, slb); }
        else { emit(L, SN_TOK_LT, start, sl, slb); }
        return;
    case '>':
        /* Never emit `>>`. Every occurrence in the corpus closes nested
         * generics (`Task<Result<unit, E>>`); two GT tokens let the parser
         * close them without re-splitting. */
        if (n1 == '=') { advance(L); emit(L, SN_TOK_GE, start, sl, slb); }
        else { emit(L, SN_TOK_GT, start, sl, slb); }
        return;
    case '&':
        if (n1 == '&') { advance(L); emit(L, SN_TOK_ANDAND, start, sl, slb); }
        else { emit(L, SN_TOK_AMP, start, sl, slb); }
        return;
    case '|':
        if (n1 == '|') { advance(L); emit(L, SN_TOK_OROR, start, sl, slb); }
        else { emit(L, SN_TOK_PIPE, start, sl, slb); }
        return;

    default: {
        SnSpan s = span_from(L, start, sl, slb);
        sn_diag_emit(L->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_CHARACTER, s,
                     "unexpected character `%.*s` in source",
                     (int)(L->pos - start), L->src + start);
        L->had_error = 1;
        emit(L, SN_TOK_ERROR, start, sl, slb);
        return;
    }
    }
}

static void scan_ident(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
    while (is_ident_cont((unsigned char)peek(L))) {
        advance(L);
    }
    size_t n = L->pos - start;
    SnTokKind k = keyword_lookup(L->src + start, n);
    if (k == SN_TOK_IDENT && n == 1 && L->src[start] == '_') {
        k = SN_TOK_UNDERSCORE;
    }
    emit(L, k, start, sl, slb);
}

int sn_lex(SnArena *arena, SnDiagSink *diag, const char *src, size_t len,
           SnTokenVec *out) {
    Lexer L;
    L.src = src;
    L.len = len;
    L.pos = 0;
    L.line = 1;
    L.line_start = 0;
    L.arena = arena;
    L.diag = diag;
    L.out = out;
    L.had_error = 0;

    out->data = NULL;
    out->len = 0;
    out->cap = 0;

    /* UTF-8 BOM */
    if (len >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        L.pos = 3;
        L.line_start = 3;
    }

    for (;;) {
        skip_trivia(&L);

        size_t start = L.pos;
        uint32_t sl = L.line;
        uint32_t slb = L.line_start;

        if (at_end(&L)) {
            SnToken t;
            t.kind = SN_TOK_EOF;
            t.span = span_from(&L, start, sl, slb);
            t.text = "";
            t.has_interpolation = 0;
            vec_push(&L, t);
            break;
        }

        char c = peek(&L);
        if (c == '"') {
            scan_string(&L, start, sl, slb);
            continue;
        }
        if (c == '\'') {
            scan_char(&L, start, sl, slb);
            continue;
        }
        if (is_digit(c)) {
            scan_number(&L, start, sl, slb);
            continue;
        }
        if (is_ident_start((unsigned char)c)) {
            scan_ident(&L, start, sl, slb);
            continue;
        }

        advance(&L);
        scan_operator(&L, c, peek(&L), start, sl, slb);
    }

    return L.had_error;
}
