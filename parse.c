#include "parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Parser band: 0100-0199 */
#define SNOVA_EXPECTED_TOKEN     100
#define SNOVA_EXPECTED_NAME      101
#define SNOVA_EXPECTED_DECL      102
#define SNOVA_EXPECTED_EXPR      103
#define SNOVA_EXPECTED_TYPE      104

typedef struct {
    const SnTokenVec *toks;
    size_t pos;
    SnArena *arena;
    SnDiagSink *diag;
    int errors;
    int panic; /* suppresses cascading diagnostics until we resynchronize */
} P;

void sn_list_push(SnArena *a, SnList *l, void *item) {
    if (l->len == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 4;
        void **nd = (void **)sn_arena_alloc(a, ncap * sizeof(void *));
        if (l->items) {
            memcpy(nd, l->items, l->len * sizeof(void *));
        }
        l->items = nd;
        l->cap = ncap;
    }
    l->items[l->len++] = item;
}

/* ── token access ─────────────────────────────────────────────────────────── */

static const SnToken *cur(P *p) { return &p->toks->data[p->pos]; }

static SnTokKind kind(P *p) { return cur(p)->kind; }

static const SnToken *peek_at(P *p, size_t n) {
    size_t i = p->pos + n;
    if (i >= p->toks->len) {
        i = p->toks->len - 1; /* EOF */
    }
    return &p->toks->data[i];
}

static int at(P *p, SnTokKind k) { return kind(p) == k; }
static int at_end_p(P *p) { return kind(p) == SN_TOK_EOF; }

static const SnToken *advance_p(P *p) {
    const SnToken *t = cur(p);
    if (!at_end_p(p)) {
        p->pos++;
    }
    return t;
}

static int accept(P *p, SnTokKind k) {
    if (at(p, k)) {
        advance_p(p);
        return 1;
    }
    return 0;
}

static void error_at(P *p, const SnToken *t, int code, const char *fmt, ...) {
    if (p->panic) {
        return;
    }
    p->errors++;
    p->panic = 1;

    /* sn_diag_emit is variadic; format here into a fixed buffer so this helper
     * can stay variadic without re-plumbing the sink. */
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sn_diag_emit(p->diag, SN_DIAG_ERROR, code, t->span, "%s", buf);
}

static int expect(P *p, SnTokKind k) {
    if (at(p, k)) {
        advance_p(p);
        return 1;
    }
    error_at(p, cur(p), SNOVA_EXPECTED_TOKEN, "expected `%s`, found `%s`",
             sn_tok_name(k), sn_tok_name(kind(p)));
    return 0;
}

/* Any keyword is a valid name wherever a name is expected.
 *
 * This is not leniency, it is the language: the corpus contains
 * `func sendNotification(method: string)` and `handle.catch(fn)`, so `method`
 * and `catch` must be usable as identifiers. Keeping the lexer context-free and
 * demoting here is what makes that work without a feedback loop. */
static const char *expect_name(P *p) {
    if (at(p, SN_TOK_IDENT) || sn_tok_is_keyword(kind(p)) ||
        at(p, SN_TOK_UNDERSCORE)) {
        return advance_p(p)->text;
    }
    error_at(p, cur(p), SNOVA_EXPECTED_NAME, "expected a name, found `%s`",
             sn_tok_name(kind(p)));
    return "<error>";
}

static int at_name_tok(SnTokKind k) {
    return k == SN_TOK_IDENT || sn_tok_is_keyword(k) || k == SN_TOK_UNDERSCORE;
}

static int at_name(P *p) {
    return at(p, SN_TOK_IDENT) || sn_tok_is_keyword(kind(p)) ||
           at(p, SN_TOK_UNDERSCORE);
}

/* ── allocation helpers ───────────────────────────────────────────────────── */

static SnExpr *new_expr(P *p, SnExprKind k, SnSpan span) {
    SnExpr *e = (SnExpr *)sn_arena_calloc(p->arena, sizeof(SnExpr));
    e->kind = k;
    e->span = span;
    return e;
}

static SnStmt *new_stmt(P *p, SnStmtKind k, SnSpan span) {
    SnStmt *s = (SnStmt *)sn_arena_calloc(p->arena, sizeof(SnStmt));
    s->kind = k;
    s->span = span;
    return s;
}

static SnDecl *new_decl(P *p, SnDeclKind k, SnSpan span) {
    SnDecl *d = (SnDecl *)sn_arena_calloc(p->arena, sizeof(SnDecl));
    d->kind = k;
    d->span = span;
    return d;
}

/* ── forward decls ────────────────────────────────────────────────────────── */

static SnType *parse_type(P *p);
static SnExpr *parse_expr(P *p);
static SnStmt *parse_stmt(P *p);
static SnStmt *parse_block(P *p);
static SnDecl *parse_decl(P *p, int in_type_body);
static SnPattern *parse_pattern(P *p);
static void parse_params(P *p, SnList *out);

/* ── names and types ──────────────────────────────────────────────────────── */

/* Dotted name: a.b.C — joined into one interned string. */
static const char *parse_qualified(P *p, SnSpan *span_out) {
    SnSpan start = cur(p)->span;
    char buf[512];
    size_t n = 0;

    const char *first = expect_name(p);
    size_t l = strlen(first);
    if (l >= sizeof(buf)) {
        l = sizeof(buf) - 1;
    }
    memcpy(buf, first, l);
    n = l;

    while (at(p, SN_TOK_DOT) && (peek_at(p, 1)->kind == SN_TOK_IDENT ||
                                 sn_tok_is_keyword(peek_at(p, 1)->kind))) {
        advance_p(p);
        const char *part = expect_name(p);
        size_t pl = strlen(part);
        if (n + 1 + pl < sizeof(buf)) {
            buf[n++] = '.';
            memcpy(buf + n, part, pl);
            n += pl;
        }
    }
    buf[n] = '\0';

    if (span_out) {
        SnSpan s = start;
        s.len = (uint32_t)((cur(p)->span.offset > start.offset)
                               ? cur(p)->span.offset - start.offset
                               : start.len);
        *span_out = s;
    }
    return sn_arena_strndup(p->arena, buf, n);
}

/* `<T, E>` — in type position `<` is unambiguously a generic opener. */
static void parse_type_args(P *p, SnList *out) {
    expect(p, SN_TOK_LT);
    if (at(p, SN_TOK_GT)) { /* `<>` */
        advance_p(p);
        return;
    }
    for (;;) {
        SnType *t = parse_type(p);
        if (t) {
            sn_list_push(p->arena, out, t);
        }
        if (accept(p, SN_TOK_COMMA)) {
            continue;
        }
        break;
    }
    /* The lexer never produces `>>`, so nested generics close with one GT each
     * and need no token splitting here. */
    expect(p, SN_TOK_GT);
}

/* Trailing `[]` marks an array type: `SelectCase<T>[]`. Applied as a wrapper so
 * the element type stays intact. */
static SnType *wrap_array_suffix(P *p, SnType *inner) {
    while (at(p, SN_TOK_LBRACKET) && peek_at(p, 1)->kind == SN_TOK_RBRACKET) {
        advance_p(p);
        advance_p(p);
        SnType *arr = (SnType *)sn_arena_calloc(p->arena, sizeof(SnType));
        arr->kind = SN_TYPE_NAME;
        arr->name = "Array";
        arr->span = inner ? inner->span : cur(p)->span;
        if (inner) {
            sn_list_push(p->arena, &arr->args, inner);
        }
        inner = arr;
    }
    return inner;
}

static SnType *parse_type(P *p) {
    SnSpan span = cur(p)->span;

    /* Reference and raw-pointer types: `&T`, `&mut T`, `*mut T`, `*const T`.
     * `mut` and `const` here are ordinary identifiers to the lexer, so they are
     * matched by text. Referentness is not modelled in the AST yet — P2 owns
     * that — but the syntax has to parse. */
    if (at(p, SN_TOK_AMP) || at(p, SN_TOK_STAR)) {
        advance_p(p);
        if (at(p, SN_TOK_IDENT) &&
            (strcmp(cur(p)->text, "mut") == 0 ||
             strcmp(cur(p)->text, "const") == 0)) {
            advance_p(p);
        }
        accept(p, SN_TOK_CONST);
        return wrap_array_suffix(p, parse_type(p));
    }

    /* Function type, `func` form: `func(DataValue) -> DataValue`. */
    if (at(p, SN_TOK_FUNC)) {
        advance_p(p);
        SnType *t = (SnType *)sn_arena_calloc(p->arena, sizeof(SnType));
        t->kind = SN_TYPE_FUNC;
        t->span = span;
        expect(p, SN_TOK_LPAREN);
        if (!at(p, SN_TOK_RPAREN)) {
            for (;;) {
                if (at_name(p) && peek_at(p, 1)->kind == SN_TOK_COLON) {
                    advance_p(p);
                    advance_p(p);
                }
                SnType *pt = parse_type(p);
                if (pt) {
                    sn_list_push(p->arena, &t->params, pt);
                }
                if (accept(p, SN_TOK_COMMA)) {
                    continue;
                }
                break;
            }
        }
        expect(p, SN_TOK_RPAREN);
        /* The return type is introduced by `:` in the `func` form — `func(): T`
         * is what the corpus writes, mirroring `method f(): T`. The `->` and
         * `~>` forms are also accepted; `(A) -> R` below is the arrow-only
         * spelling. Absent all three, the type is `func(...)` returning unit. */
        if (accept(p, SN_TOK_COLON) || accept(p, SN_TOK_ARROW) ||
            accept(p, SN_TOK_TILDE_ARROW)) {
            t->ret = parse_type(p);
        }
        return wrap_array_suffix(p, t);
    }

    /* Function type: `(A, B) -> R`, used for callback parameters. */
    if (at(p, SN_TOK_LPAREN)) {
        SnType *t = (SnType *)sn_arena_calloc(p->arena, sizeof(SnType));
        t->kind = SN_TYPE_FUNC;
        t->span = span;
        advance_p(p);
        if (!at(p, SN_TOK_RPAREN)) {
            for (;;) {
                /* A function-type parameter may be written `name: T` or bare
                 * `T`; both occur in the corpus. */
                if (at_name(p) && peek_at(p, 1)->kind == SN_TOK_COLON) {
                    advance_p(p);
                    advance_p(p);
                }
                SnType *pt = parse_type(p);
                if (pt) {
                    sn_list_push(p->arena, &t->params, pt);
                }
                if (accept(p, SN_TOK_COMMA)) {
                    continue;
                }
                break;
            }
        }
        expect(p, SN_TOK_RPAREN);
        /* `(T) ~> Channel<R>` is the pulsar-stream form of `(T) -> R`. */
        if (!accept(p, SN_TOK_TILDE_ARROW)) {
            expect(p, SN_TOK_ARROW);
        }
        t->ret = parse_type(p);
        return wrap_array_suffix(p, t);
    }

    if (!at_name(p)) {
        error_at(p, cur(p), SNOVA_EXPECTED_TYPE, "expected a type, found `%s`",
                 sn_tok_name(kind(p)));
        return NULL;
    }

    SnType *t = (SnType *)sn_arena_calloc(p->arena, sizeof(SnType));
    t->kind = SN_TYPE_NAME;
    t->name = parse_qualified(p, &t->span);
    if (at(p, SN_TOK_LT)) {
        parse_type_args(p, &t->args);
    }
    /* `T?` optional shorthand, when present, is just sugar recorded on the name. */
    accept(p, SN_TOK_QUESTION);
    return wrap_array_suffix(p, t);
}

/* ── patterns ─────────────────────────────────────────────────────────────── */

static SnPattern *parse_pattern(P *p) {
    SnSpan span = cur(p)->span;
    SnPattern *pat = (SnPattern *)sn_arena_calloc(p->arena, sizeof(SnPattern));
    pat->span = span;

    if (at(p, SN_TOK_UNDERSCORE)) {
        advance_p(p);
        pat->kind = SN_PAT_WILDCARD;
        return pat;
    }

    if (at(p, SN_TOK_INT) || at(p, SN_TOK_LONG) || at(p, SN_TOK_DOUBLE) ||
        at(p, SN_TOK_DECIMAL) || at(p, SN_TOK_STRING) || at(p, SN_TOK_CHAR) ||
        at(p, SN_TOK_TRUE) || at(p, SN_TOK_FALSE) || at(p, SN_TOK_MINUS)) {
        pat->kind = SN_PAT_LITERAL;
        pat->literal = parse_expr(p);
        return pat;
    }

    pat->name = parse_qualified(p, &pat->span);
    if (at(p, SN_TOK_LT)) { /* Variant<T>(x) */
        SnList throwaway = {0};
        parse_type_args(p, &throwaway);
    }
    if (accept(p, SN_TOK_LPAREN)) {
        pat->kind = SN_PAT_VARIANT;
        if (!at(p, SN_TOK_RPAREN)) {
            for (;;) {
                SnPattern *sub = parse_pattern(p);
                if (sub) {
                    sn_list_push(p->arena, &pat->subs, sub);
                }
                if (accept(p, SN_TOK_COMMA)) {
                    continue;
                }
                break;
            }
        }
        expect(p, SN_TOK_RPAREN);
    } else {
        /* A bare dotted name is a variant (None, DateTimeError.OutOfRange);
         * a bare simple name binds. Which one it is needs the resolver, so
         * record BINDING and let P2 reclassify. */
        pat->kind = strchr(pat->name, '.') ? SN_PAT_VARIANT : SN_PAT_BINDING;
    }
    return pat;
}

/* ── expressions ──────────────────────────────────────────────────────────── */

typedef struct {
    int lbp; /* left binding power; 0 means "not an infix operator" */
    int right_assoc;
} BindPower;

static BindPower infix_power(SnTokKind k) {
    BindPower b = {0, 0};
    switch (k) {
    case SN_TOK_ASSIGN:
    case SN_TOK_PLUS_EQ: case SN_TOK_MINUS_EQ:
    case SN_TOK_STAR_EQ: case SN_TOK_SLASH_EQ:
        b.lbp = 1; b.right_assoc = 1; break;
    case SN_TOK_OROR:    b.lbp = 2; break;
    case SN_TOK_QQ:      b.lbp = 2; break; /* `a ?? b` null-coalescing */
    case SN_TOK_ANDAND:  b.lbp = 3; break;
    case SN_TOK_PIPE:    b.lbp = 4; break;
    case SN_TOK_CARET:   b.lbp = 5; break;
    case SN_TOK_AMP:     b.lbp = 6; break;
    case SN_TOK_EQ: case SN_TOK_NE: b.lbp = 7; break;
    case SN_TOK_LT: case SN_TOK_GT:
    case SN_TOK_LE: case SN_TOK_GE: b.lbp = 8; break;
    /* 9 is reserved for shift — see at_shift(). */
    case SN_TOK_PLUS: case SN_TOK_MINUS: b.lbp = 10; break;
    case SN_TOK_STAR: case SN_TOK_SLASH: case SN_TOK_PERCENT: b.lbp = 11; break;
    case SN_TOK_AS: case SN_TOK_IS: b.lbp = 12; break;
    default: break;
    }
    return b;
}

#define SN_SHIFT_BP 9

/* Shift operators exist, but the lexer never emits `>>` because that sequence
 * also closes nested generics (`Task<Result<unit, E>>`). Both readings are real:
 * libs/ uses `>>` only for generics, compiler/src has `(hi >> 16) & 0xFF`.
 *
 * They are told apart by position, not by spelling: only in infix position,
 * with the two angle brackets physically adjacent, is this a shift. Type
 * argument lists consume one `>` at a time and never reach here. */
static int at_shift(P *p, SnTokKind *op_out) {
    SnTokKind k = kind(p);
    if (k != SN_TOK_GT && k != SN_TOK_LT) {
        return 0;
    }
    const SnToken *a = cur(p);
    const SnToken *b = peek_at(p, 1);
    if (b->kind != k) {
        return 0;
    }
    if (a->span.offset + a->span.len != b->span.offset) {
        return 0; /* `a > > b` is not a shift */
    }
    *op_out = k;
    return 1;
}

static void parse_call_args(P *p, SnExpr *call) {
    expect(p, SN_TOK_LPAREN);
    if (!at(p, SN_TOK_RPAREN)) {
        for (;;) {
            /* Named argument: `description: "..."`. Only the value is kept as
             * the argument expression; names matter to decorators, which parse
             * their own arguments. */
            if (at_name(p) && peek_at(p, 1)->kind == SN_TOK_COLON) {
                advance_p(p);
                advance_p(p);
            }
            SnExpr *a = parse_expr(p);
            if (a) {
                sn_list_push(p->arena, &call->args, a);
            }
            if (accept(p, SN_TOK_COMMA)) {
                if (at(p, SN_TOK_RPAREN)) { /* trailing comma */
                    break;
                }
                continue;
            }
            break;
        }
    }
    expect(p, SN_TOK_RPAREN);
}

/* Is the token run starting at `(` a lambda parameter list?
 * Scans to the matching `)` and checks for a following `->`. Cheap and exact,
 * which beats guessing from the first token. */
static int looks_like_lambda(P *p) {
    size_t i = p->pos;
    if (p->toks->data[i].kind != SN_TOK_LPAREN) {
        return 0;
    }
    int depth = 0;
    for (; i < p->toks->len; i++) {
        SnTokKind k = p->toks->data[i].kind;
        if (k == SN_TOK_LPAREN) depth++;
        else if (k == SN_TOK_RPAREN) {
            depth--;
            if (depth == 0) {
                return p->toks->data[i + 1].kind == SN_TOK_ARROW;
            }
        } else if (k == SN_TOK_EOF) {
            break;
        }
    }
    return 0;
}

static SnExpr *parse_lambda(P *p) {
    SnSpan span = cur(p)->span;
    SnExpr *e = new_expr(p, SN_EXPR_LAMBDA, span);
    expect(p, SN_TOK_LPAREN);
    if (!at(p, SN_TOK_RPAREN)) {
        for (;;) {
            SnParam *prm = (SnParam *)sn_arena_calloc(p->arena, sizeof(SnParam));
            prm->span = cur(p)->span;
            prm->name = expect_name(p);
            if (accept(p, SN_TOK_COLON)) {
                prm->type = parse_type(p);
            }
            sn_list_push(p->arena, &e->params, prm);
            if (accept(p, SN_TOK_COMMA)) {
                continue;
            }
            break;
        }
    }
    expect(p, SN_TOK_RPAREN);
    expect(p, SN_TOK_ARROW);
    /* A lambda may declare its return type before a block body:
     * `(row: int) -> bool { ... }`. That is ambiguous with an expression body
     * whose first token also starts a type — `(x) -> foo` — so speculate: parse
     * a type and keep it only if a `{` follows. Otherwise rewind and treat the
     * whole thing as an expression body. Same idiom as try_generic_call_args. */
    if (!at(p, SN_TOK_LBRACE)) {
        size_t save_pos = p->pos;
        int save_errors = p->errors;
        int save_panic = p->panic;
        p->panic = 1;
        SnType *ret = parse_type(p);
        int ok = ret != NULL && at(p, SN_TOK_LBRACE);
        p->panic = save_panic;
        p->errors = save_errors;
        if (ok) {
            e->type = ret;
        } else {
            p->pos = save_pos;
        }
    }
    if (at(p, SN_TOK_LBRACE)) {
        e->body = parse_block(p);
    } else {
        e->value = parse_expr(p);
    }
    return e;
}

static SnExpr *parse_match_expr(P *p);

static SnExpr *parse_primary(P *p) {
    SnSpan span = cur(p)->span;

    switch (kind(p)) {
    case SN_TOK_INT:     { SnExpr *e = new_expr(p, SN_EXPR_INT, span);     e->text = advance_p(p)->text; return e; }
    case SN_TOK_LONG:    { SnExpr *e = new_expr(p, SN_EXPR_LONG, span);    e->text = advance_p(p)->text; return e; }
    case SN_TOK_DOUBLE:  { SnExpr *e = new_expr(p, SN_EXPR_DOUBLE, span);  e->text = advance_p(p)->text; return e; }
    case SN_TOK_DECIMAL: { SnExpr *e = new_expr(p, SN_EXPR_DECIMAL, span); e->text = advance_p(p)->text; return e; }
    case SN_TOK_CHAR:    { SnExpr *e = new_expr(p, SN_EXPR_CHAR, span);    e->text = advance_p(p)->text; return e; }
    case SN_TOK_STRING: {
        SnExpr *e = new_expr(p, SN_EXPR_STRING, span);
        e->interpolated = cur(p)->has_interpolation;
        e->text = advance_p(p)->text;
        return e;
    }
    case SN_TOK_TRUE:
    case SN_TOK_FALSE: {
        SnExpr *e = new_expr(p, SN_EXPR_BOOL, span);
        e->text = advance_p(p)->text;
        return e;
    }
    case SN_TOK_THIS: {
        advance_p(p);
        return new_expr(p, SN_EXPR_THIS, span);
    }
    case SN_TOK_AWAIT: {
        advance_p(p);
        SnExpr *e = new_expr(p, SN_EXPR_AWAIT, span);
        e->lhs = parse_expr(p);
        return e;
    }
    case SN_TOK_MATCH:
        return parse_match_expr(p);
    case SN_TOK_FUNC: {
        /* Anonymous function expression: `Async.spawn(func(): int { return 42 })`.
         * Same shape as a lambda, so it reuses SN_EXPR_LAMBDA.
         *
         * `func` is also an ordinary identifier in the corpus (`func.name`,
         * `func.line`), so this only fires when a parameter list follows. */
        SnTokKind n1 = peek_at(p, 1)->kind;
        int is_anon_fn = (n1 == SN_TOK_LPAREN) ||
                         (at_name_tok(n1) && peek_at(p, 2)->kind == SN_TOK_LPAREN);
        if (!is_anon_fn) {
            SnExpr *id = new_expr(p, SN_EXPR_IDENT, span);
            id->text = advance_p(p)->text;
            return id;
        }
        advance_p(p);
        SnExpr *e = new_expr(p, SN_EXPR_LAMBDA, span);
        if (at_name(p)) {
            advance_p(p); /* optional name */
        }
        parse_params(p, &e->params);
        if (accept(p, SN_TOK_COLON) || accept(p, SN_TOK_ARROW) ||
            accept(p, SN_TOK_TILDE_ARROW)) {
            parse_type(p);
        }
        if (at(p, SN_TOK_LBRACE)) {
            e->body = parse_block(p);
        } else if (accept(p, SN_TOK_ARROW)) {
            e->value = parse_expr(p);
        }
        return e;
    }
    case SN_TOK_BANG:
    case SN_TOK_MINUS:
    case SN_TOK_PLUS:
    case SN_TOK_TILDE: {
        SnTokKind op = kind(p);
        advance_p(p);
        SnExpr *e = new_expr(p, SN_EXPR_UNARY, span);
        e->op = op;
        e->lhs = parse_primary(p);
        return e;
    }
    case SN_TOK_LBRACKET: { /* array literal */
        advance_p(p);
        SnExpr *e = new_expr(p, SN_EXPR_ARRAY, span);
        if (!at(p, SN_TOK_RBRACKET)) {
            for (;;) {
                SnExpr *el = parse_expr(p);
                if (el) {
                    sn_list_push(p->arena, &e->args, el);
                }
                if (accept(p, SN_TOK_COMMA)) {
                    if (at(p, SN_TOK_RBRACKET)) {
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        expect(p, SN_TOK_RBRACKET);
        return e;
    }
    case SN_TOK_LPAREN: {
        if (looks_like_lambda(p)) {
            return parse_lambda(p);
        }
        advance_p(p);
        SnExpr *inner = parse_expr(p);
        expect(p, SN_TOK_RPAREN);
        return inner;
    }
    default:
        break;
    }

    if (at_name(p)) {
        SnExpr *e = new_expr(p, SN_EXPR_IDENT, span);
        e->text = advance_p(p)->text;
        return e;
    }

    error_at(p, cur(p), SNOVA_EXPECTED_EXPR, "expected an expression, found `%s`",
             sn_tok_name(kind(p)));
    return NULL;
}

/* `foo<int>(x)` is a generic call; `a < b` is a comparison. Distinguishing them
 * needs lookahead, so speculatively parse the type-argument list and require a
 * `(` right after. If that fails, rewind and let `<` be an operator. */
static int try_generic_call_args(P *p, SnExpr *call) {
    size_t save_pos = p->pos;
    int save_errors = p->errors;
    int save_panic = p->panic;

    /* Suppress diagnostics while speculating: a failed guess is not an error. */
    p->panic = 1;
    SnList args = {0};
    parse_type_args(p, &args);
    /* A generic instantiation is followed either by a call — `foo<int>(x)` — or
     * by a member access on the type itself: `Array<Post>.new()`. Anything else
     * means the `<` was a comparison. */
    int ok = !at(p, SN_TOK_EOF) &&
             (at(p, SN_TOK_LPAREN) || at(p, SN_TOK_DOT));
    p->panic = save_panic;
    p->errors = save_errors;

    if (!ok) {
        p->pos = save_pos;
        return 0;
    }
    call->type_args = args;
    return 1;
}

static SnExpr *parse_postfix(P *p, SnExpr *lhs) {
    for (;;) {
        if (!lhs) {
            return NULL;
        }
        SnSpan span = cur(p)->span;

        if (at(p, SN_TOK_DOT)) {
            advance_p(p);
            SnExpr *e = new_expr(p, SN_EXPR_MEMBER, span);
            e->lhs = lhs;
            e->text = expect_name(p); /* keywords are legal member names */
            lhs = e;
            continue;
        }
        if (at(p, SN_TOK_LPAREN)) {
            SnExpr *e = new_expr(p, SN_EXPR_CALL, span);
            e->lhs = lhs;
            parse_call_args(p, e);
            lhs = e;
            continue;
        }
        /* `LspDiagnostic[]()` constructs an empty array of that type. The
         * `[]` belongs to the type, not to an index expression. */
        if (at(p, SN_TOK_LBRACKET) && peek_at(p, 1)->kind == SN_TOK_RBRACKET &&
            peek_at(p, 2)->kind == SN_TOK_LPAREN) {
            advance_p(p);
            advance_p(p);
            continue;
        }
        if (at(p, SN_TOK_LBRACKET)) {
            advance_p(p);
            SnExpr *e = new_expr(p, SN_EXPR_INDEX, span);
            e->lhs = lhs;
            e->rhs = parse_expr(p);
            expect(p, SN_TOK_RBRACKET);
            lhs = e;
            continue;
        }
        if (at(p, SN_TOK_LT) &&
            (lhs->kind == SN_EXPR_IDENT || lhs->kind == SN_EXPR_MEMBER)) {
            SnExpr *e = new_expr(p, SN_EXPR_CALL, span);
            e->lhs = lhs;
            if (try_generic_call_args(p, e)) {
                if (at(p, SN_TOK_LPAREN)) {
                    parse_call_args(p, e);
                    lhs = e;
                } else {
                    /* `Array<Post>.new()` — the generic arguments belong to the
                     * type, and the postfix loop picks up `.new()` next pass. */
                    e->kind = SN_EXPR_IDENT;
                    e->text = lhs->text;
                    lhs = e;
                }
                continue;
            }
        }
        return lhs;
    }
}

static SnExpr *parse_binary(P *p, int min_bp) {
    SnExpr *lhs = parse_postfix(p, parse_primary(p));

    for (;;) {
        SnTokKind shift_op;
        if (at_shift(p, &shift_op) && SN_SHIFT_BP >= min_bp) {
            SnSpan span = cur(p)->span;
            advance_p(p);
            advance_p(p);
            SnExpr *e = new_expr(p, SN_EXPR_BINARY, span);
            e->op = shift_op;
            e->lhs = lhs;
            e->rhs = parse_binary(p, SN_SHIFT_BP + 1);
            lhs = e;
            continue;
        }

        SnTokKind op = kind(p);
        BindPower bp = infix_power(op);
        if (bp.lbp == 0 || bp.lbp < min_bp) {
            break;
        }
        SnSpan span = cur(p)->span;
        advance_p(p);

        if (op == SN_TOK_AS || op == SN_TOK_IS) {
            SnExpr *e = new_expr(p, op == SN_TOK_AS ? SN_EXPR_CAST : SN_EXPR_IS,
                                 span);
            e->lhs = lhs;
            e->type = parse_type(p);
            lhs = e;
            continue;
        }

        int next_bp = bp.right_assoc ? bp.lbp : bp.lbp + 1;
        SnExpr *rhs = parse_binary(p, next_bp);

        SnExprKind ek = (bp.lbp == 1) ? SN_EXPR_ASSIGN : SN_EXPR_BINARY;
        SnExpr *e = new_expr(p, ek, span);
        e->op = op;
        e->lhs = lhs;
        e->rhs = rhs;
        lhs = e;
    }
    return lhs;
}

/* Can this token begin an expression? Used to tell `cond ? a : b` from the
 * postfix `expr?` error-propagation form. */
static int starts_expr(SnTokKind k) {
    switch (k) {
    case SN_TOK_RPAREN: case SN_TOK_RBRACE: case SN_TOK_RBRACKET:
    case SN_TOK_COMMA:  case SN_TOK_SEMI:   case SN_TOK_EOF:
    case SN_TOK_COLON:  case SN_TOK_FATARROW:
        return 0;
    default:
        return 1;
    }
}

static SnExpr *parse_expr(P *p) {
    SnExpr *e = parse_binary(p, 1);

    if (at(p, SN_TOK_QUESTION)) {
        SnSpan span = cur(p)->span;
        if (starts_expr(peek_at(p, 1)->kind)) {
            /* Ternary: `parseInt(e) >= 0 ? intToString(...) : "-"`. Modelled as
             * a one-armed match-free conditional using the binary slots. */
            advance_p(p);
            SnExpr *t = new_expr(p, SN_EXPR_BINARY, span);
            t->op = SN_TOK_QUESTION;
            t->lhs = e;
            SnExpr *then_e = parse_expr(p);
            expect(p, SN_TOK_COLON);
            SnExpr *else_e = parse_expr(p);
            SnExpr *pair = new_expr(p, SN_EXPR_BINARY, span);
            pair->op = SN_TOK_COLON;
            pair->lhs = then_e;
            pair->rhs = else_e;
            t->rhs = pair;
            return t;
        }
        /* Postfix `?`: `Db.connect(url)?` propagates an error result. */
        advance_p(p);
        SnExpr *u = new_expr(p, SN_EXPR_UNARY, span);
        u->op = SN_TOK_QUESTION;
        u->lhs = e;
        return u;
    }
    return e;
}

/* ── match ────────────────────────────────────────────────────────────────── */

/* Arms are newline-separated, not comma-separated, and an arm body is either an
 * expression or a block. A trailing comma is tolerated because some corpus files
 * use one. */
static void parse_match_arms(P *p, SnList *out) {
    expect(p, SN_TOK_LBRACE);
    while (!at(p, SN_TOK_RBRACE) && !at_end_p(p)) {
        SnMatchArm *arm = (SnMatchArm *)sn_arena_calloc(p->arena, sizeof(SnMatchArm));
        arm->span = cur(p)->span;
        arm->pattern = parse_pattern(p);
        /* Optional guard: `pattern if <expr> -> body`. The arm reads as a
         * lambda whose parameters are the pattern bindings, so the guard is
         * the validation applied to those bindings before the body runs. */
        if (accept(p, SN_TOK_IF)) {
            arm->guard = parse_expr(p);
        }
        if (!expect(p, SN_TOK_ARROW)) {
            break;
        }
        if (at(p, SN_TOK_LBRACE)) {
            arm->body = parse_block(p);
        } else if (at(p, SN_TOK_RETURN) || at(p, SN_TOK_THROW) ||
                   at(p, SN_TOK_BREAK) || at(p, SN_TOK_CONTINUE)) {
            arm->body = parse_stmt(p);
        } else {
            arm->value = parse_expr(p);
        }
        accept(p, SN_TOK_COMMA);
        sn_list_push(p->arena, out, arm);
        p->panic = 0;
    }
    expect(p, SN_TOK_RBRACE);
}

static SnExpr *parse_match_expr(P *p) {
    SnSpan span = cur(p)->span;
    expect(p, SN_TOK_MATCH);
    SnExpr *e = new_expr(p, SN_EXPR_MATCH, span);
    e->lhs = parse_expr(p);
    parse_match_arms(p, &e->arms);
    return e;
}

/* ── statements ───────────────────────────────────────────────────────────── */

static SnStmt *parse_block(P *p) {
    SnSpan span = cur(p)->span;
    SnStmt *s = new_stmt(p, SN_STMT_BLOCK, span);
    expect(p, SN_TOK_LBRACE);
    while (!at(p, SN_TOK_RBRACE) && !at_end_p(p)) {
        SnStmt *st = parse_stmt(p);
        if (st) {
            sn_list_push(p->arena, &s->stmts, st);
        }
        if (p->panic) {
            /* Resynchronize at a statement or block boundary. */
            while (!at_end_p(p) && !at(p, SN_TOK_RBRACE) && !at(p, SN_TOK_SEMI)) {
                advance_p(p);
            }
            accept(p, SN_TOK_SEMI);
            p->panic = 0;
        }
    }
    expect(p, SN_TOK_RBRACE);
    return s;
}

static SnStmt *parse_binding(P *p, SnStmtKind k) {
    SnSpan span = cur(p)->span;
    advance_p(p); /* let / var */
    SnStmt *s = new_stmt(p, k, span);
    s->name = expect_name(p);
    if (accept(p, SN_TOK_COLON)) {
        s->type = parse_type(p);
    }
    if (accept(p, SN_TOK_ASSIGN)) {
        s->expr = parse_expr(p);
    }
    accept(p, SN_TOK_SEMI);
    return s;
}

static SnStmt *parse_stmt(P *p) {
    SnSpan span = cur(p)->span;

    switch (kind(p)) {
    case SN_TOK_LET: return parse_binding(p, SN_STMT_LET);
    case SN_TOK_VAR: return parse_binding(p, SN_STMT_VAR);
    case SN_TOK_LBRACE: return parse_block(p);

    case SN_TOK_RETURN: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_RETURN, span);
        /* `return` with no value is legal; detect by what follows. */
        if (!at(p, SN_TOK_RBRACE) && !at(p, SN_TOK_SEMI) && !at_end_p(p) &&
            cur(p)->span.line == span.line) {
            s->expr = parse_expr(p);
        }
        accept(p, SN_TOK_SEMI);
        return s;
    }
    case SN_TOK_IF: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_IF, span);
        /* Do not treat a leading `(` as a condition wrapper: in
         * `if (isGet || isPost) && selected(x) {` the parens are a sub-expression,
         * not a wrapper, and consuming them here strands the `&&`. parse_expr
         * already parses `(...)` as a primary, so the fully-parenthesised form
         * `if (cond) {` falls out of the same path. */
        s->expr = parse_expr(p);
        s->then_br = at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
        if (accept(p, SN_TOK_ELSE)) {
            s->else_br = at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
        }
        return s;
    }
    case SN_TOK_WHILE: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_WHILE, span);
        /* Same reasoning as the `if` arm above. */
        s->expr = parse_expr(p);
        s->then_br = at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
        return s;
    }
    case SN_TOK_FOR: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_FOR, span);
        int paren = accept(p, SN_TOK_LPAREN);
        accept(p, SN_TOK_LET); /* `for (let x in xs)` */
        accept(p, SN_TOK_VAR);
        s->name = expect_name(p);
        if (accept(p, SN_TOK_COLON)) {
            s->type = parse_type(p);
        }
        /* `for v <~ ch` iterates a channel; `for v in xs` iterates a collection.
         * Both are for-loops over a source, so they share SN_STMT_FOR — which of
         * the two it is follows from the type of `s->expr`, and that is P2's
         * call, not the parser's. */
        if (!accept(p, SN_TOK_RECV_BIND)) {
            expect(p, SN_TOK_IN);
        }
        s->expr = parse_expr(p);
        if (paren) {
            expect(p, SN_TOK_RPAREN);
        }
        s->then_br = at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
        return s;
    }
    case SN_TOK_MATCH: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_MATCH, span);
        s->expr = parse_expr(p);
        parse_match_arms(p, &s->arms);
        return s;
    }
    case SN_TOK_BREAK: {
        advance_p(p);
        accept(p, SN_TOK_SEMI);
        return new_stmt(p, SN_STMT_BREAK, span);
    }
    case SN_TOK_CONTINUE: {
        advance_p(p);
        accept(p, SN_TOK_SEMI);
        return new_stmt(p, SN_STMT_CONTINUE, span);
    }
    case SN_TOK_THROW: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_THROW, span);
        s->expr = parse_expr(p);
        accept(p, SN_TOK_SEMI);
        return s;
    }
    case SN_TOK_DEFER: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_DEFER, span);
        s->then_br = at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
        return s;
    }
    case SN_TOK_PULSAR: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_PULSAR, span);
        if (at(p, SN_TOK_LBRACE)) {
            s->then_br = parse_block(p);
        } else {
            s->expr = parse_expr(p);
            accept(p, SN_TOK_SEMI);
        }
        return s;
    }
    case SN_TOK_TRY: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_TRY, span);
        s->then_br = at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
        while (at(p, SN_TOK_CATCH)) {
            advance_p(p);
            int paren = accept(p, SN_TOK_LPAREN);
            SnStmt *c = NULL;
            if (at_name(p)) {
                const char *nm = expect_name(p);
                if (accept(p, SN_TOK_COLON)) {
                    parse_type(p);
                }
                if (paren) {
                    expect(p, SN_TOK_RPAREN);
                }
                c = parse_block(p);
                c->name = nm;
            } else {
                if (paren) {
                    expect(p, SN_TOK_RPAREN);
                }
                c = parse_block(p);
            }
            sn_list_push(p->arena, &s->catches, c);
        }
        return s;
    }
    default:
        break;
    }

    /* Receive-bind: `x <~ expr`, `ok, value <~ ch.tryReceive()`. Declares
     * mutable bindings with inferred types, and receives from a channel when the
     * right-hand side is a Channel<T> — the two readings differ only by type, so
     * the parser emits SN_STMT_VAR either way and leaves the choice to P2.
     * Detected by scanning `name (, name)*` up to a `<~`, which keeps a plain
     * expression statement starting with an identifier untouched. */
    if (at_name(p)) {
        size_t i = p->pos;
        for (;;) {
            if (!at_name_tok(p->toks->data[i].kind)) { i = 0; break; }
            i++;
            if (p->toks->data[i].kind == SN_TOK_COMMA) { i++; continue; }
            break;
        }
        if (i != 0 && p->toks->data[i].kind == SN_TOK_RECV_BIND) {
            SnStmt *s = new_stmt(p, SN_STMT_VAR, span);
            s->name = expect_name(p);
            while (accept(p, SN_TOK_COMMA)) {
                const char *extra = expect_name(p);
                if (extra) {
                    sn_list_push(p->arena, &s->extra_names, (void *)(uintptr_t)extra);
                }
            }
            expect(p, SN_TOK_RECV_BIND);
            s->expr = parse_expr(p);
            accept(p, SN_TOK_SEMI);
            return s;
        }
    }

    SnStmt *s = new_stmt(p, SN_STMT_EXPR, span);
    s->expr = parse_expr(p);
    accept(p, SN_TOK_SEMI);
    return s;
}

/* ── declarations ─────────────────────────────────────────────────────────── */

static void parse_decorators(P *p, SnList *out) {
    while (at(p, SN_TOK_AT)) {
        SnSpan span = cur(p)->span;
        advance_p(p);
        SnDecorator *d = (SnDecorator *)sn_arena_calloc(p->arena, sizeof(SnDecorator));
        d->span = span;
        d->name = parse_qualified(p, NULL);
        if (at(p, SN_TOK_LT)) { /* @SqlQuery<UserRow>(...) */
            SnList dargs = {0};
            parse_type_args(p, &dargs);
        }

        if (accept(p, SN_TOK_LPAREN)) {
            if (!at(p, SN_TOK_RPAREN)) {
                for (;;) {
                    const char *argname = NULL;
                    /* `description: "..."` and `description = "..."` are both
                     * used in the corpus. */
                    if (at_name(p) && (peek_at(p, 1)->kind == SN_TOK_COLON ||
                                       peek_at(p, 1)->kind == SN_TOK_ASSIGN)) {
                        argname = cur(p)->text;
                        advance_p(p);
                        advance_p(p);
                    }

                    /* `returnType: List<T>` passes a bare TYPE, not a value.
                     * Try the type reading first when the shape looks like a
                     * generic or dotted type; otherwise read an expression. */
                    SnExpr *val = NULL;
                    SnType *ty = NULL;
                    size_t save = p->pos;
                    int save_err = p->errors;
                    int save_panic = p->panic;
                    if (at_name(p)) {
                        p->panic = 1;
                        ty = parse_type(p);
                        int type_ok = ty && (at(p, SN_TOK_COMMA) || at(p, SN_TOK_RPAREN));
                        p->panic = save_panic;
                        p->errors = save_err;
                        if (!type_ok) {
                            p->pos = save;
                            ty = NULL;
                        }
                    }
                    if (!ty) {
                        val = parse_expr(p);
                    }

                    sn_list_push(p->arena, &d->arg_names, (void *)argname);
                    sn_list_push(p->arena, &d->arg_values, val);
                    sn_list_push(p->arena, &d->arg_types, ty);

                    if (accept(p, SN_TOK_COMMA)) {
                        if (at(p, SN_TOK_RPAREN)) {
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            expect(p, SN_TOK_RPAREN);
        }
        sn_list_push(p->arena, out, d);
    }
}

static void parse_generic_params(P *p, SnList *out) {
    expect(p, SN_TOK_LT);
    if (accept(p, SN_TOK_GT)) {
        return;
    }
    for (;;) {
        const char *n = expect_name(p);
        sn_list_push(p->arena, out, (void *)n);
        /* `<T: Bound>` — the bound is parsed and discarded until P2. */
        if (accept(p, SN_TOK_COLON)) {
            parse_type(p);
        }
        if (accept(p, SN_TOK_COMMA)) {
            continue;
        }
        break;
    }
    expect(p, SN_TOK_GT);
}

static void parse_params(P *p, SnList *out) {
    expect(p, SN_TOK_LPAREN);
    if (at(p, SN_TOK_RPAREN)) {
        advance_p(p);
        return;
    }
    for (;;) {
        SnParam *prm = (SnParam *)sn_arena_calloc(p->arena, sizeof(SnParam));
        prm->span = cur(p)->span;
        prm->name = expect_name(p);
        if (accept(p, SN_TOK_COLON)) {
            prm->type = parse_type(p);
        }
        /* Default value: `frame: TabularFrame = TabularFrame.new()`. */
        if (accept(p, SN_TOK_ASSIGN)) {
            prm->def = parse_expr(p);
        }
        sn_list_push(p->arena, out, prm);
        if (accept(p, SN_TOK_COMMA)) {
            if (at(p, SN_TOK_RPAREN)) {
                break;
            }
            continue;
        }
        break;
    }
    expect(p, SN_TOK_RPAREN);
}

static void parse_type_body(P *p, SnDecl *d) {
    expect(p, SN_TOK_LBRACE);
    while (!at(p, SN_TOK_RBRACE) && !at_end_p(p)) {
        SnDecl *m = parse_decl(p, 1);
        if (m) {
            sn_list_push(p->arena, &d->members, m);
        }
        if (p->panic) {
            while (!at_end_p(p) && !at(p, SN_TOK_RBRACE) &&
                   !at(p, SN_TOK_METHOD) && !at(p, SN_TOK_LET) &&
                   !at(p, SN_TOK_VAR) && !at(p, SN_TOK_AT)) {
                advance_p(p);
            }
            p->panic = 0;
        }
    }
    expect(p, SN_TOK_RBRACE);
}

static void parse_enum_body(P *p, SnDecl *d) {
    expect(p, SN_TOK_LBRACE);
    while (!at(p, SN_TOK_RBRACE) && !at_end_p(p)) {
        /* An enum body may hold variants and also methods (DateTimeError has
         * both shapes across the corpus). */
        if (at(p, SN_TOK_METHOD) || at(p, SN_TOK_STATIC) || at(p, SN_TOK_AT) ||
            at(p, SN_TOK_FUNC) || at(p, SN_TOK_PUBLIC) || at(p, SN_TOK_PRIVATE)) {
            SnDecl *m = parse_decl(p, 1);
            if (m) {
                sn_list_push(p->arena, &d->members, m);
            }
        } else {
            SnDecl *v = new_decl(p, SN_DECL_VARIANT, cur(p)->span);
            v->name = expect_name(p);
            if (at(p, SN_TOK_LPAREN)) {
                parse_params(p, &v->params);
            }
            sn_list_push(p->arena, &d->variants, v);
        }
        accept(p, SN_TOK_COMMA);
        if (p->panic) {
            while (!at_end_p(p) && !at(p, SN_TOK_RBRACE) && !at(p, SN_TOK_COMMA)) {
                advance_p(p);
            }
            accept(p, SN_TOK_COMMA);
            p->panic = 0;
        }
    }
    expect(p, SN_TOK_RBRACE);
}

static SnDecl *parse_decl(P *p, int in_type_body) {
    SnList decorators = {0};
    parse_decorators(p, &decorators);

    SnSpan span = cur(p)->span;
    SnVisibility vis = SN_VIS_DEFAULT;
    uint8_t is_static = 0, is_override = 0, is_async = 0;

    for (;;) {
        if (at(p, SN_TOK_PUBLIC))    { vis = SN_VIS_PUBLIC;    advance_p(p); continue; }
        if (at(p, SN_TOK_PRIVATE))   { vis = SN_VIS_PRIVATE;   advance_p(p); continue; }
        if (at(p, SN_TOK_PROTECTED)) { vis = SN_VIS_PROTECTED; advance_p(p); continue; }
        if (at(p, SN_TOK_STATIC))    { is_static = 1;   advance_p(p); continue; }
        if (at(p, SN_TOK_OVERRIDE))  { is_override = 1; advance_p(p); continue; }
        /* `async` is a modifier only when a declaration keyword follows;
         * otherwise it is an ordinary identifier. */
        if (at(p, SN_TOK_ASYNC) && (peek_at(p, 1)->kind == SN_TOK_METHOD ||
                                    peek_at(p, 1)->kind == SN_TOK_FUNC)) {
            is_async = 1; advance_p(p); continue;
        }
        /* `pulsar func worker(): unit { }` — pulsar is a declaration modifier
         * here, and a statement keyword elsewhere. */
        if (at(p, SN_TOK_PULSAR) && (peek_at(p, 1)->kind == SN_TOK_FUNC ||
                                     peek_at(p, 1)->kind == SN_TOK_METHOD)) {
            advance_p(p);
            continue;
        }
        /* Soft modifiers: `data class User`, `unsafe method read()`, `sealed`,
         * `abstract`, `final`. None are keywords in the lexer, so they are
         * matched by text and only when a declaration keyword follows — which
         * keeps `data` usable as an ordinary identifier everywhere else. */
        if (at(p, SN_TOK_IDENT)) {
            const char *w = cur(p)->text;
            SnTokKind nk = peek_at(p, 1)->kind;
            int decl_follows =
                nk == SN_TOK_CLASS || nk == SN_TOK_STRUCT || nk == SN_TOK_ENUM ||
                nk == SN_TOK_INTERFACE || nk == SN_TOK_METHOD ||
                nk == SN_TOK_FUNC || nk == SN_TOK_LET || nk == SN_TOK_VAR;
            if (decl_follows &&
                (strcmp(w, "data") == 0 || strcmp(w, "unsafe") == 0 ||
                 strcmp(w, "sealed") == 0 || strcmp(w, "abstract") == 0 ||
                 strcmp(w, "final") == 0 || strcmp(w, "open") == 0 ||
                 strcmp(w, "internal") == 0)) {
                advance_p(p);
                continue;
            }
        }
        break;
    }

    /* A `data class` body declares fields as bare `name: Type` with no
     * `let`/`var`. Recognised only inside a type body, so a stray identifier at
     * top level still reports a missing declaration. */
    if (in_type_body && at_name(p) && peek_at(p, 1)->kind == SN_TOK_COLON &&
        !at(p, SN_TOK_METHOD) && !at(p, SN_TOK_FUNC)) {
        SnDecl *f = new_decl(p, SN_DECL_FIELD, span);
        f->decorators = decorators;
        f->vis = vis;
        f->name = expect_name(p);
        expect(p, SN_TOK_COLON);
        f->type = parse_type(p);
        if (accept(p, SN_TOK_ASSIGN)) {
            f->init = parse_expr(p);
        }
        accept(p, SN_TOK_COMMA);
        accept(p, SN_TOK_SEMI);
        return f;
    }

    /* Type alias: `public type RowDecoder<T> = func(SqlRow) -> Result<T, E>`.
     * `type` is not a lexer keyword — it is a common identifier — so it is
     * recognised by text plus the shape that follows. */
    if (at(p, SN_TOK_IDENT) && strcmp(cur(p)->text, "type") == 0 &&
        at_name_tok(peek_at(p, 1)->kind)) {
        advance_p(p);
        SnDecl *ta = new_decl(p, SN_DECL_TYPEALIAS, span);
        ta->decorators = decorators;
        ta->vis = vis;
        ta->name = expect_name(p);
        if (at(p, SN_TOK_LT)) {
            parse_generic_params(p, &ta->generics);
        }
        expect(p, SN_TOK_ASSIGN);
        ta->type = parse_type(p);
        accept(p, SN_TOK_SEMI);
        return ta;
    }

    SnDeclKind k;
    switch (kind(p)) {
    case SN_TOK_CLASS:     k = SN_DECL_CLASS;     break;
    case SN_TOK_STRUCT:    k = SN_DECL_STRUCT;    break;
    case SN_TOK_ENUM:      k = SN_DECL_ENUM;      break;
    case SN_TOK_INTERFACE: k = SN_DECL_INTERFACE; break;
    case SN_TOK_METHOD:    k = SN_DECL_METHOD;    break;
    case SN_TOK_FUNC:      k = SN_DECL_FUNC;      break;
    case SN_TOK_LET:
    case SN_TOK_VAR:       k = SN_DECL_FIELD;     break;
    case SN_TOK_CONST:     k = SN_DECL_CONST;     break;
    default:
        error_at(p, cur(p), SNOVA_EXPECTED_DECL,
                 "expected a declaration, found `%s`", sn_tok_name(kind(p)));
        return NULL;
    }

    SnDecl *d = new_decl(p, k, span);
    d->decorators = decorators;
    d->vis = vis;
    d->is_static = is_static;
    d->is_override = is_override;
    d->is_async = is_async;

    if (k == SN_DECL_FIELD) {
        d->is_mutable = at(p, SN_TOK_VAR) ? 1u : 0u;
    }
    advance_p(p); /* the declaration keyword */

    switch (k) {
    case SN_DECL_CLASS:
    case SN_DECL_STRUCT:
    case SN_DECL_INTERFACE:
        d->name = expect_name(p);
        if (at(p, SN_TOK_LT)) {
            parse_generic_params(p, &d->generics);
        }
        /* `class Foo: Bar` / `class Foo(a: int)` primary-constructor and
         * supertype forms are read and kept only as shape until P2. */
        if (at(p, SN_TOK_LPAREN)) {
            parse_params(p, &d->params);
        }
        if (accept(p, SN_TOK_COLON)) {
            for (;;) {
                SnType *sup = parse_type(p);
                if (sup) {
                    sn_list_push(p->arena, &d->supertypes, sup);
                }
                if (accept(p, SN_TOK_COMMA)) {
                    continue;
                }
                break;
            }
        }
        /* A bodyless type is an opaque native handle: `@native struct Scheduler`.
         * Requiring `@native` on it is a semantic rule owned by P2, exactly as
         * for a bodyless method — the grammar only records the absence. */
        if (at(p, SN_TOK_LBRACE)) {
            parse_type_body(p, d);
        }
        return d;

    case SN_DECL_ENUM:
        d->name = expect_name(p);
        if (at(p, SN_TOK_LT)) {
            parse_generic_params(p, &d->generics);
        }
        parse_enum_body(p, d);
        return d;

    case SN_DECL_METHOD:
    case SN_DECL_FUNC:
        d->name = expect_name(p);
        if (at(p, SN_TOK_LT)) {
            parse_generic_params(p, &d->generics);
        }
        parse_params(p, &d->params);
        /* Return type is spelled `: T` or `-> T`; both occur in the corpus. */
        if (accept(p, SN_TOK_COLON) || accept(p, SN_TOK_ARROW) ||
            accept(p, SN_TOK_TILDE_ARROW)) {
            d->ret = parse_type(p);
        }
        if (at(p, SN_TOK_LBRACE)) {
            d->body = parse_block(p);
        } else if (accept(p, SN_TOK_ASSIGN)) {
            /* Expression body: `func main(): Int = 0`. The right-hand side may
             * itself be a block (`method foo(): int = { ... }`). */
            if (at(p, SN_TOK_LBRACE)) {
                d->body = parse_block(p);
                return d;
            }
            /* Wrapped in an implicit return so later phases see one shape. */
            SnStmt *ret = new_stmt(p, SN_STMT_RETURN, cur(p)->span);
            ret->expr = parse_expr(p);
            SnStmt *blk = new_stmt(p, SN_STMT_BLOCK, ret->span);
            sn_list_push(p->arena, &blk->stmts, ret);
            d->body = blk;
        }
        /* Still no body means the declaration is backed by @native. That is a
         * semantic rule checked in P2, not a syntax error here. */
        return d;

    case SN_DECL_FIELD:
    case SN_DECL_CONST:
        d->name = expect_name(p);
        if (accept(p, SN_TOK_COLON)) {
            d->type = parse_type(p);
        }
        if (accept(p, SN_TOK_ASSIGN)) {
            d->init = parse_expr(p);
        }
        accept(p, SN_TOK_SEMI);
        return d;

    default:
        break;
    }

    (void)in_type_body;
    return d;
}

/* ── compilation unit ─────────────────────────────────────────────────────── */

SnExpr *sn_parse_expr_only(SnArena *arena, SnDiagSink *diag,
                           const SnTokenVec *toks) {
    P p;
    p.toks = toks;
    p.pos = 0;
    p.arena = arena;
    p.diag = diag;
    p.errors = 0;
    p.panic = 0;
    SnExpr *e = parse_expr(&p);
    return p.errors ? NULL : e;
}

int sn_parse(SnArena *arena, SnDiagSink *diag, const SnTokenVec *toks,
             SnUnit *out) {
    P p;
    p.toks = toks;
    p.pos = 0;
    p.arena = arena;
    p.diag = diag;
    p.errors = 0;
    p.panic = 0;

    memset(out, 0, sizeof(*out));

    if (at(&p, SN_TOK_PACKAGE)) {
        out->package_span = cur(&p)->span;
        advance_p(&p);
        out->package = parse_qualified(&p, NULL);
        accept(&p, SN_TOK_SEMI);
    }

    while (at(&p, SN_TOK_IMPORT)) {
        advance_p(&p);
        const char *name = parse_qualified(&p, NULL);
        sn_list_push(arena, &out->imports, (void *)name);
        accept(&p, SN_TOK_SEMI);
    }

    while (!at_end_p(&p)) {
        /* A file may declare more than one package. The cross-package
         * visibility fixtures (tests/compile-pass/visibility_*_cross_package)
         * put provider and consumer packages in a single file, so a second
         * `package` opens a new section rather than ending the unit. Only the
         * first name is recorded here; P2 owns per-section scoping. */
        if (at(&p, SN_TOK_PACKAGE)) {
            advance_p(&p);
            const char *name = parse_qualified(&p, NULL);
            if (!out->package) {
                out->package = name;
            }
            accept(&p, SN_TOK_SEMI);
            continue;
        }
        if (at(&p, SN_TOK_IMPORT)) {
            advance_p(&p);
            const char *name = parse_qualified(&p, NULL);
            sn_list_push(arena, &out->imports, (void *)name);
            accept(&p, SN_TOK_SEMI);
            continue;
        }

        SnDecl *d = parse_decl(&p, 0);
        if (d) {
            sn_list_push(arena, &out->decls, d);
        }
        if (p.panic) {
            /* Skip to something that can begin a top-level declaration. */
            while (!at_end_p(&p) && !at(&p, SN_TOK_CLASS) &&
                   !at(&p, SN_TOK_STRUCT) && !at(&p, SN_TOK_ENUM) &&
                   !at(&p, SN_TOK_INTERFACE) && !at(&p, SN_TOK_FUNC) &&
                   !at(&p, SN_TOK_AT) && !at(&p, SN_TOK_PUBLIC) &&
                   !at(&p, SN_TOK_PRIVATE)) {
                advance_p(&p);
            }
            p.panic = 0;
        }
    }

    return p.errors ? 1 : 0;
}
