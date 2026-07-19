#include "lex.h"

#include <stdlib.h>
#include <string.h>

/* ── keyword table ────────────────────────────────────────────────────────── */

typedef struct {
    const char *word;
    SnTokKind kind;
} Keyword;

/* Only words that actually introduce syntax. Deliberately absent, because the
 * corpus uses them as ordinary identifiers: and, or, not, when, select, spawn,
 * yield, short, byte, char, data, value, key, name, get, new-as-member.
 * Primitive type names (int, long, double, decimal, bool, string, unit) are NOT
 * keywords either — they are ordinary type references resolved by name, which
 * is what lets `builtin.types` declare them in .snova. */
static const Keyword KEYWORDS[] = {
    {"package", SN_TOK_PACKAGE},     {"import", SN_TOK_IMPORT},
    {"class", SN_TOK_CLASS},         {"struct", SN_TOK_STRUCT},
    {"enum", SN_TOK_ENUM},           {"interface", SN_TOK_INTERFACE},
    {"method", SN_TOK_METHOD},       {"func", SN_TOK_FUNC},
    {"static", SN_TOK_STATIC},       {"public", SN_TOK_PUBLIC},
    {"private", SN_TOK_PRIVATE},     {"protected", SN_TOK_PROTECTED},
    {"override", SN_TOK_OVERRIDE},   {"let", SN_TOK_LET},
    {"var", SN_TOK_VAR},             {"const", SN_TOK_CONST},
    {"return", SN_TOK_RETURN},       {"if", SN_TOK_IF},
    {"else", SN_TOK_ELSE},           {"while", SN_TOK_WHILE},
    {"for", SN_TOK_FOR},             {"in", SN_TOK_IN},
    {"match", SN_TOK_MATCH},         {"break", SN_TOK_BREAK},
    {"continue", SN_TOK_CONTINUE},   {"try", SN_TOK_TRY},
    {"catch", SN_TOK_CATCH},         {"throw", SN_TOK_THROW},
    {"defer", SN_TOK_DEFER},         {"async", SN_TOK_ASYNC},
    {"await", SN_TOK_AWAIT},         {"pulsar", SN_TOK_PULSAR},
    {"this", SN_TOK_THIS},           {"new", SN_TOK_NEW},
    {"as", SN_TOK_AS},               {"is", SN_TOK_IS},
    {"true", SN_TOK_TRUE},           {"false", SN_TOK_FALSE},
};

static SnTokKind keyword_lookup(const char *s, size_t n) {
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strlen(KEYWORDS[i].word) == n && memcmp(KEYWORDS[i].word, s, n) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return SN_TOK_IDENT;
}

int sn_tok_is_keyword(SnTokKind k) {
    return k >= SN_TOK_PACKAGE && k <= SN_TOK_FALSE;
}

const char *sn_tok_name(SnTokKind k) {
    switch (k) {
    case SN_TOK_EOF: return "end of file";
    case SN_TOK_ERROR: return "invalid token";
    case SN_TOK_IDENT: return "identifier";
    case SN_TOK_INT: return "int literal";
    case SN_TOK_LONG: return "long literal";
    case SN_TOK_DOUBLE: return "double literal";
    case SN_TOK_DECIMAL: return "decimal literal";
    case SN_TOK_STRING: return "string literal";
    case SN_TOK_CHAR: return "char literal";
    default: break;
    }
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (KEYWORDS[i].kind == k) {
            return KEYWORDS[i].word;
        }
    }
    switch (k) {
    case SN_TOK_LPAREN: return "(";
    case SN_TOK_RPAREN: return ")";
    case SN_TOK_LBRACE: return "{";
    case SN_TOK_RBRACE: return "}";
    case SN_TOK_LBRACKET: return "[";
    case SN_TOK_RBRACKET: return "]";
    case SN_TOK_COMMA: return ",";
    case SN_TOK_DOT: return ".";
    case SN_TOK_COLON: return ":";
    case SN_TOK_SEMI: return ";";
    case SN_TOK_AT: return "@";
    case SN_TOK_QUESTION: return "?";
    case SN_TOK_QQ: return "??";
    case SN_TOK_COLONCOLON: return "::";
    case SN_TOK_UNDERSCORE: return "_";
    case SN_TOK_PLUS: return "+";
    case SN_TOK_MINUS: return "-";
    case SN_TOK_STAR: return "*";
    case SN_TOK_SLASH: return "/";
    case SN_TOK_PERCENT: return "%";
    case SN_TOK_ASSIGN: return "=";
    case SN_TOK_EQ: return "==";
    case SN_TOK_NE: return "!=";
    case SN_TOK_LT: return "<";
    case SN_TOK_GT: return ">";
    case SN_TOK_LE: return "<=";
    case SN_TOK_GE: return ">=";
    case SN_TOK_ANDAND: return "&&";
    case SN_TOK_OROR: return "||";
    case SN_TOK_BANG: return "!";
    case SN_TOK_AMP: return "&";
    case SN_TOK_PIPE: return "|";
    case SN_TOK_CARET: return "^";
    case SN_TOK_TILDE: return "~";
    case SN_TOK_ARROW: return "->";
    case SN_TOK_TILDE_ARROW: return "~>";
    case SN_TOK_FATARROW: return "=>";
    case SN_TOK_PLUS_EQ: return "+=";
    case SN_TOK_MINUS_EQ: return "-=";
    case SN_TOK_STAR_EQ: return "*=";
    case SN_TOK_SLASH_EQ: return "/=";
    default: return "token";
    }
}

/* ── lexer state ──────────────────────────────────────────────────────────── */

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

static int at_end(const Lexer *L) { return L->pos >= L->len; }
static char peek(const Lexer *L) { return at_end(L) ? '\0' : L->src[L->pos]; }
static char peek2(const Lexer *L) {
    return L->pos + 1 >= L->len ? '\0' : L->src[L->pos + 1];
}

static char advance(Lexer *L) {
    char c = L->src[L->pos++];
    if (c == '\n') {
        L->line++;
        L->line_start = (uint32_t)L->pos;
    }
    return c;
}

static SnSpan span_from(const Lexer *L, size_t start, uint32_t start_line,
                        uint32_t start_line_begin) {
    SnSpan s;
    s.offset = (uint32_t)start;
    s.len = (uint32_t)(L->pos - start);
    s.line = start_line;
    s.col = (uint32_t)(start - start_line_begin) + 1u;
    return s;
}

static void vec_push(Lexer *L, SnToken t) {
    SnTokenVec *v = L->out;
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 256;
        SnToken *nd = (SnToken *)sn_arena_alloc(L->arena, ncap * sizeof(SnToken));
        if (v->data) {
            memcpy(nd, v->data, v->len * sizeof(SnToken));
        }
        v->data = nd;
        v->cap = ncap;
    }
    v->data[v->len++] = t;
}

static void emit(Lexer *L, SnTokKind kind, size_t start, uint32_t sl,
                 uint32_t slb) {
    SnToken t;
    t.kind = kind;
    t.span = span_from(L, start, sl, slb);
    t.text = sn_arena_strndup(L->arena, L->src + start, L->pos - start);
    t.has_interpolation = 0;
    vec_push(L, t);
}

static int is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c >= 0x80; /* UTF-8 continuation: identifiers may be non-ASCII */
}

static int is_ident_cont(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* ── literals ─────────────────────────────────────────────────────────────── */

/* Scans a string literal, tracking `${...}` interpolation.
 *
 * `$$` is a literal `$` and is NOT interpolation. `${` opens an expression that
 * runs to its matching `}`; nesting is tracked by brace depth so that
 * `"${map.get("k")}"` and `"${ {a:1} }"` both close correctly. The expression
 * text itself is left in the raw token — parsing it belongs to the parser. */
static void scan_string(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
    int has_interp = 0;
    advance(L); /* opening quote */

    while (!at_end(L)) {
        char c = peek(L);

        if (c == '"') {
            advance(L);
            SnToken t;
            t.kind = SN_TOK_STRING;
            t.span = span_from(L, start, sl, slb);
            t.text = sn_arena_strndup(L->arena, L->src + start, L->pos - start);
            t.has_interpolation = (uint8_t)has_interp;
            vec_push(L, t);
            return;
        }

        if (c == '\n') {
            /* Unterminated: report at the opening quote and stop before the
             * newline so the rest of the file still lexes cleanly. */
            SnSpan s = span_from(L, start, sl, slb);
            s.len = 1;
            sn_diag_emit(L->diag, SN_DIAG_ERROR, SNOVA_UNTERMINATED_STRING, s,
                         "unterminated string literal");
            L->had_error = 1;
            emit(L, SN_TOK_ERROR, start, sl, slb);
            return;
        }

        if (c == '\\') {
            advance(L);
            if (!at_end(L)) {
                advance(L);
            }
            continue;
        }

        if (c == '$') {
            if (peek2(L) == '$') { /* literal dollar */
                advance(L);
                advance(L);
                continue;
            }
            if (peek2(L) == '{') {
                has_interp = 1;
                advance(L); /* $ */
                advance(L); /* { */
                int depth = 1;
                int in_str = 0;
                while (!at_end(L) && depth > 0) {
                    char d = peek(L);
                    if (in_str) {
                        if (d == '\\') {
                            advance(L);
                            if (!at_end(L)) advance(L);
                            continue;
                        }
                        if (d == '"') in_str = 0;
                        if (d == '\n') break;
                        advance(L);
                        continue;
                    }
                    if (d == '"') { in_str = 1; advance(L); continue; }
                    if (d == '{') depth++;
                    if (d == '}') depth--;
                    if (d == '\n') break;
                    advance(L);
                }
                if (depth != 0) {
                    SnSpan s = span_from(L, start, sl, slb);
                    s.len = 1;
                    sn_diag_emit(L->diag, SN_DIAG_ERROR,
                                 SNOVA_UNTERMINATED_INTERP, s,
                                 "unterminated `${...}` interpolation in string "
                                 "literal");
                    L->had_error = 1;
                    emit(L, SN_TOK_ERROR, start, sl, slb);
                    return;
                }
                continue;
            }
        }

        advance(L);
    }

    SnSpan s = span_from(L, start, sl, slb);
    s.len = 1;
    sn_diag_emit(L->diag, SN_DIAG_ERROR, SNOVA_UNTERMINATED_STRING, s,
                 "unterminated string literal at end of file");
    L->had_error = 1;
    emit(L, SN_TOK_ERROR, start, sl, slb);
}

static void scan_char(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
    advance(L); /* opening quote */
    while (!at_end(L) && peek(L) != '\'' && peek(L) != '\n') {
        if (peek(L) == '\\') {
            advance(L);
            if (at_end(L)) break;
        }
        advance(L);
    }
    if (peek(L) != '\'') {
        SnSpan s = span_from(L, start, sl, slb);
        s.len = 1;
        sn_diag_emit(L->diag, SN_DIAG_ERROR, SNOVA_UNTERMINATED_CHAR, s,
                     "unterminated char literal");
        L->had_error = 1;
        emit(L, SN_TOK_ERROR, start, sl, slb);
        return;
    }
    advance(L);
    emit(L, SN_TOK_CHAR, start, sl, slb);
}

static void scan_number(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
    SnTokKind kind = SN_TOK_INT;

    if (peek(L) == '0' && (peek2(L) == 'x' || peek2(L) == 'X')) {
        advance(L);
        advance(L);
        if (!is_hex(peek(L))) {
            SnSpan s = span_from(L, start, sl, slb);
            sn_diag_emit(L->diag, SN_DIAG_ERROR, SNOVA_INVALID_NUMBER, s,
                         "hex literal has no digits after `0x`");
            L->had_error = 1;
            emit(L, SN_TOK_ERROR, start, sl, slb);
            return;
        }
        while (is_hex(peek(L)) || peek(L) == '_') {
            advance(L);
        }
    } else {
        while (is_digit(peek(L)) || peek(L) == '_') {
            advance(L);
        }
        /* A `.` is a fraction only when a digit follows; otherwise it is member
         * access on an int literal (`1.toString()`), which must not be eaten. */
        if (peek(L) == '.' && is_digit(peek2(L))) {
            kind = SN_TOK_DOUBLE;
            advance(L);
            while (is_digit(peek(L)) || peek(L) == '_') {
                advance(L);
            }
        }
        if (peek(L) == 'e' || peek(L) == 'E') {
            char sign = peek2(L);
            size_t save = L->pos;
            uint32_t save_line = L->line;
            uint32_t save_lb = L->line_start;
            advance(L);
            if (sign == '+' || sign == '-') {
                advance(L);
            }
            if (is_digit(peek(L))) {
                kind = SN_TOK_DOUBLE;
                while (is_digit(peek(L))) {
                    advance(L);
                }
            } else {
                /* Not an exponent — `e` belongs to a following identifier. */
                L->pos = save;
                L->line = save_line;
                L->line_start = save_lb;
            }
        }
    }

    /* Type suffix: 42L, 1.5d. Only consume when not followed by more identifier
     * characters, so `1.toString` and `0xFFm4` stay unambiguous. */
    char sfx = peek(L);
    if ((sfx == 'L' || sfx == 'l') && !is_ident_cont((unsigned char)peek2(L))) {
        advance(L);
        kind = SN_TOK_LONG;
    } else if ((sfx == 'd' || sfx == 'D') &&
               !is_ident_cont((unsigned char)peek2(L))) {
        advance(L);
        kind = SN_TOK_DECIMAL;
    }

    emit(L, kind, start, sl, slb);
}

/* ── driver ───────────────────────────────────────────────────────────────── */

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
        /* whitespace and comments */
        for (;;) {
            char c = peek(&L);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance(&L);
                continue;
            }
            if (c == '/' && peek2(&L) == '/') {
                while (!at_end(&L) && peek(&L) != '\n') {
                    advance(&L);
                }
                continue;
            }
            /* Block comments: the corpus contains zero of them, but accepting
             * the slash-star form costs nothing and keeps the lexer
             * forward-compatible with the grammar decision pending in
             * plan.md §3. */
            if (c == '/' && peek2(&L) == '*') {
                advance(&L);
                advance(&L);
                while (!at_end(&L) && !(peek(&L) == '*' && peek2(&L) == '/')) {
                    advance(&L);
                }
                if (!at_end(&L)) {
                    advance(&L);
                    advance(&L);
                }
                continue;
            }
            break;
        }

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
            while (is_ident_cont((unsigned char)peek(&L))) {
                advance(&L);
            }
            size_t n = L.pos - start;
            SnTokKind k = keyword_lookup(src + start, n);
            if (k == SN_TOK_IDENT && n == 1 && src[start] == '_') {
                k = SN_TOK_UNDERSCORE;
            }
            emit(&L, k, start, sl, slb);
            continue;
        }

        advance(&L);
        char n1 = peek(&L);

        switch (c) {
        case '(': emit(&L, SN_TOK_LPAREN, start, sl, slb); continue;
        case ')': emit(&L, SN_TOK_RPAREN, start, sl, slb); continue;
        case '{': emit(&L, SN_TOK_LBRACE, start, sl, slb); continue;
        case '}': emit(&L, SN_TOK_RBRACE, start, sl, slb); continue;
        case '[': emit(&L, SN_TOK_LBRACKET, start, sl, slb); continue;
        case ']': emit(&L, SN_TOK_RBRACKET, start, sl, slb); continue;
        case ',': emit(&L, SN_TOK_COMMA, start, sl, slb); continue;
        case '.': emit(&L, SN_TOK_DOT, start, sl, slb); continue;
        case ':':
            if (n1 == ':') { advance(&L); emit(&L, SN_TOK_COLONCOLON, start, sl, slb); }
            else { emit(&L, SN_TOK_COLON, start, sl, slb); }
            continue;
        case ';': emit(&L, SN_TOK_SEMI, start, sl, slb); continue;
        case '@': emit(&L, SN_TOK_AT, start, sl, slb); continue;
        case '?':
            if (n1 == '?') { advance(&L); emit(&L, SN_TOK_QQ, start, sl, slb); }
            else { emit(&L, SN_TOK_QUESTION, start, sl, slb); }
            continue;
        case '~':
            if (n1 == '>') { advance(&L); emit(&L, SN_TOK_TILDE_ARROW, start, sl, slb); }
            else { emit(&L, SN_TOK_TILDE, start, sl, slb); }
            continue;
        case '^': emit(&L, SN_TOK_CARET, start, sl, slb); continue;

        case '+':
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_PLUS_EQ, start, sl, slb); }
            else { emit(&L, SN_TOK_PLUS, start, sl, slb); }
            continue;
        case '-':
            if (n1 == '>') { advance(&L); emit(&L, SN_TOK_ARROW, start, sl, slb); }
            else if (n1 == '=') { advance(&L); emit(&L, SN_TOK_MINUS_EQ, start, sl, slb); }
            else { emit(&L, SN_TOK_MINUS, start, sl, slb); }
            continue;
        case '*':
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_STAR_EQ, start, sl, slb); }
            else { emit(&L, SN_TOK_STAR, start, sl, slb); }
            continue;
        case '/':
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_SLASH_EQ, start, sl, slb); }
            else { emit(&L, SN_TOK_SLASH, start, sl, slb); }
            continue;
        case '%': emit(&L, SN_TOK_PERCENT, start, sl, slb); continue;

        case '=':
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_EQ, start, sl, slb); }
            else if (n1 == '>') { advance(&L); emit(&L, SN_TOK_FATARROW, start, sl, slb); }
            else { emit(&L, SN_TOK_ASSIGN, start, sl, slb); }
            continue;
        case '!':
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_NE, start, sl, slb); }
            else { emit(&L, SN_TOK_BANG, start, sl, slb); }
            continue;
        case '<':
            /* No `<<`: Snovalang has no shift operators, and `<` is the generic
             * opener. Always one token. */
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_LE, start, sl, slb); }
            else { emit(&L, SN_TOK_LT, start, sl, slb); }
            continue;
        case '>':
            /* Never emit `>>`. Every occurrence in the corpus closes nested
             * generics (`Task<Result<unit, E>>`); two GT tokens let the parser
             * close them without re-splitting. */
            if (n1 == '=') { advance(&L); emit(&L, SN_TOK_GE, start, sl, slb); }
            else { emit(&L, SN_TOK_GT, start, sl, slb); }
            continue;
        case '&':
            if (n1 == '&') { advance(&L); emit(&L, SN_TOK_ANDAND, start, sl, slb); }
            else { emit(&L, SN_TOK_AMP, start, sl, slb); }
            continue;
        case '|':
            if (n1 == '|') { advance(&L); emit(&L, SN_TOK_OROR, start, sl, slb); }
            else { emit(&L, SN_TOK_PIPE, start, sl, slb); }
            continue;

        default: {
            SnSpan s = span_from(&L, start, sl, slb);
            sn_diag_emit(L.diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_CHARACTER, s,
                         "unexpected character `%.*s` in source",
                         (int)(L.pos - start), src + start);
            L.had_error = 1;
            emit(&L, SN_TOK_ERROR, start, sl, slb);
            continue;
        }
        }
    }

    return L.had_error;
}
