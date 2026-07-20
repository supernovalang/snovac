/* lex_literal.c — string, char and number literals. */
#include "lex_internal.h"

static void literal_error(Lexer *L, int code, size_t start, uint32_t sl,
                          uint32_t slb, const char *msg) {
    SnSpan s = span_from(L, start, sl, slb);
    s.len = 1;
    sn_diag_emit(L->diag, SN_DIAG_ERROR, code, s, "%s", msg);
    L->had_error = 1;
    emit(L, SN_TOK_ERROR, start, sl, slb);
}

/* Consumes a `${...}` run, starting just past the `{`. Nesting is tracked by
 * brace depth so `"${map.get("k")}"` and `"${ {a:1} }"` both close correctly;
 * quotes inside the expression do not terminate the outer literal. Returns 0
 * when the interpolation is unterminated. */
static int scan_interpolation(Lexer *L) {
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
    return depth == 0;
}

/* Scans a string literal, tracking `${...}` interpolation.
 *
 * `$$` is a literal `$` and is NOT interpolation. `${` opens an expression that
 * runs to its matching `}`. The expression text itself is left in the raw
 * token — parsing it belongs to the parser. */
void scan_string(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
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
            literal_error(L, SNOVA_UNTERMINATED_STRING, start, sl, slb,
                          "unterminated string literal");
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
                if (!scan_interpolation(L)) {
                    literal_error(L, SNOVA_UNTERMINATED_INTERP, start, sl, slb,
                                  "unterminated `${...}` interpolation in "
                                  "string literal");
                    return;
                }
                continue;
            }
        }

        advance(L);
    }

    literal_error(L, SNOVA_UNTERMINATED_STRING, start, sl, slb,
                  "unterminated string literal at end of file");
}

void scan_char(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
    advance(L); /* opening quote */
    while (!at_end(L) && peek(L) != '\'' && peek(L) != '\n') {
        if (peek(L) == '\\') {
            advance(L);
            if (at_end(L)) break;
        }
        advance(L);
    }
    if (peek(L) != '\'') {
        literal_error(L, SNOVA_UNTERMINATED_CHAR, start, sl, slb,
                      "unterminated char literal");
        return;
    }
    advance(L);
    emit(L, SN_TOK_CHAR, start, sl, slb);
}

/* `1e9` is a double, but `1e` followed by identifier characters is an int and a
 * separate name — so the exponent is scanned speculatively and rewound. */
static int scan_exponent(Lexer *L) {
    char sign = peek2(L);
    size_t save = L->pos;
    uint32_t save_line = L->line;
    uint32_t save_lb = L->line_start;
    advance(L);
    if (sign == '+' || sign == '-') {
        advance(L);
    }
    if (is_digit(peek(L))) {
        while (is_digit(peek(L))) {
            advance(L);
        }
        return 1;
    }
    L->pos = save;
    L->line = save_line;
    L->line_start = save_lb;
    return 0;
}

void scan_number(Lexer *L, size_t start, uint32_t sl, uint32_t slb) {
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
        if ((peek(L) == 'e' || peek(L) == 'E') && scan_exponent(L)) {
            kind = SN_TOK_DOUBLE;
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
