/* eval_string.c — string literal decoding and `${...}` interpolation. */
#include "eval_internal.h"

#include "lex.h"
#include "parse.h"

/* A growable arena-backed character buffer. Interpolation can expand a literal
 * well past its source length, so the buffer has to grow on demand. */
typedef struct {
    Interp *in;
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_reserve(StrBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) {
        return;
    }
    size_t ncap = b->cap ? b->cap * 2 : 32;
    while (ncap < b->len + extra + 1) {
        ncap *= 2;
    }
    char *nd = (char *)sn_arena_alloc(b->in->arena, ncap);
    memcpy(nd, b->data, b->len);
    b->data = nd;
    b->cap = ncap;
}

static void sb_push(StrBuf *b, char c) {
    sb_reserve(b, 1);
    b->data[b->len++] = c;
}

static void sb_push_str(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

/* Interpolation needs to evaluate an expression written inside a literal. The
 * inner source is lexed and parsed on demand with the ordinary front-end — no
 * ad-hoc string scanning, so `"${a + b}"` and `"${f(x)}"` behave exactly like
 * the same code written outside the string. */
static Value eval_interp_source(Interp *in, Env *env, const char *src,
                                size_t len, SnSpan span) {
    char *buf = sn_arena_strndup(in->arena, src, len);
    SnTokenVec toks;
    if (sn_lex(in->arena, in->diag, buf, len, &toks) != 0) {
        return v_unit();
    }
    SnExpr *e = sn_parse_expr_only(in->arena, in->diag, &toks);
    if (!e) {
        rt_error(in, SNOVA_UNSUPPORTED, span,
                 "could not parse the expression inside `${...}`");
        return v_unit();
    }
    return eval_expr(in, env, e);
}

static char decode_escape(char d) {
    switch (d) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '0':  return '\0';
    case '\\': return '\\';
    case '"':  return '"';
    default:   return d;
    }
}

/* Consumes `${...}` starting at the `$` in `raw[i]`, appends the evaluated
 * result, and returns the index of the closing `}` (or of the last character
 * scanned when the brace is missing). */
static size_t append_interpolation(StrBuf *b, Env *env, const SnExpr *e,
                                   const char *raw, size_t n, size_t i) {
    size_t start = i + 2;
    int depth = 1;
    size_t j = start;
    for (; j < n && depth > 0; j++) {
        if (raw[j] == '{') depth++;
        else if (raw[j] == '}') depth--;
        if (depth == 0) break;
    }
    Value v = eval_interp_source(b->in, env, raw + start, j - start, e->span);
    sb_push_str(b, to_string(b->in, v, e->span));
    return j;
}

const char *decode_string(Interp *in, Env *env, const SnExpr *e) {
    const char *raw = e->text;
    size_t n = strlen(raw);
    if (n >= 2 && raw[0] == '"') {
        raw++;
        n -= 2;
    }

    StrBuf b;
    b.in = in;
    b.len = 0;
    b.cap = n + 1;
    b.data = (char *)sn_arena_alloc(in->arena, b.cap);

    for (size_t i = 0; i < n; i++) {
        char c = raw[i];
        if (c == '\\' && i + 1 < n) {
            sb_push(&b, decode_escape(raw[++i]));
            continue;
        }
        /* `$$` is a literal `$` and never opens interpolation — see
         * docs/spec-json-interpolation.md. */
        if (c == '$' && i + 1 < n && raw[i + 1] == '$') {
            sb_push(&b, '$');
            i++;
            continue;
        }
        if (c == '$' && i + 1 < n && raw[i + 1] == '{') {
            i = append_interpolation(&b, env, e, raw, n, i);
            continue;
        }
        sb_push(&b, c);
    }

    b.data[b.len] = '\0';
    return b.data;
}
