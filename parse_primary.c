/* parse_primary.c — atoms of the expression grammar: literals, identifiers,
 * lambdas, anonymous functions, and the `if` / `match` expression forms. */
#include "parse_internal.h"

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
        SnDiagSink *save_diag = p->diag;
        p->diag = NULL;
        SnType *ret = parse_type(p);
        int ok = (p->errors == save_errors) && ret != NULL && at(p, SN_TOK_LBRACE);
        p->diag = save_diag;
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

/* Anonymous function expression: `Async.spawn(func(): int { return 42 })`.
 * Same shape as a lambda, so it reuses SN_EXPR_LAMBDA.
 *
 * `func` is also an ordinary identifier in the corpus (`func.name`,
 * `func.line`), so this only fires when a parameter list follows. */
static SnExpr *parse_anon_fn(P *p, SnSpan span) {
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

/* ── match ────────────────────────────────────────────────────────────────── */

/* Arms are newline-separated, not comma-separated, and an arm body is either an
 * expression or a block. A trailing comma is tolerated because some corpus files
 * use one. */
void parse_match_arms(P *p, SnList *out) {
    PCtx ctx = ctx_clear(p);
    expect(p, SN_TOK_LBRACE);
    while (!at(p, SN_TOK_RBRACE) && !at_end_p(p)) {
        SnMatchArm *arm =
            (SnMatchArm *)sn_arena_calloc(p->arena, sizeof(SnMatchArm));
        arm->span = cur(p)->span;
        arm->pattern = parse_pattern(p);
        /* Optional guard: `pattern if <expr> -> body`. The arm reads as a
         * lambda whose parameters are the pattern bindings, so the guard is
         * the validation applied to those bindings before the body runs. */
        if (accept(p, SN_TOK_IF)) {
            int save_nsl = p->no_struct_lit;
            p->no_struct_lit = 1; /* the `{` after a guard is never a literal */
            arm->guard = parse_expr(p);
            p->no_struct_lit = save_nsl;
        }
        if (!expect(p, SN_TOK_ARROW)) {
            break;
        }
        if (at(p, SN_TOK_LBRACE)) {
            arm->body = parse_block(p);
        } else {
            /* An arm body ends at the newline, so the next arm's pattern is not
             * swallowed as a continuation — see parse_binary. */
            int save_nl = p->nl_stops_infix;
            p->nl_stops_infix = 1;
            if (at(p, SN_TOK_RETURN) || at(p, SN_TOK_THROW) ||
                at(p, SN_TOK_BREAK) || at(p, SN_TOK_CONTINUE)) {
                arm->body = parse_stmt(p);
            } else {
                arm->value = parse_expr(p);
            }
            p->nl_stops_infix = save_nl;
        }
        accept(p, SN_TOK_COMMA);
        sn_list_push(p->arena, out, arm);
        p->panic = 0;
    }
    expect(p, SN_TOK_RBRACE);
    ctx_restore(p, ctx);
}

static SnExpr *parse_match_expr(P *p) {
    SnSpan span = cur(p)->span;
    expect(p, SN_TOK_MATCH);
    SnExpr *e = new_expr(p, SN_EXPR_MATCH, span);
    int save_nsl = p->no_struct_lit;
    p->no_struct_lit = 1; /* `match x {` — the `{` opens the arm list */
    e->lhs = parse_expr(p);
    p->no_struct_lit = save_nsl;
    parse_match_arms(p, &e->arms);
    return e;
}

/* `if c { a } else { b }` in expression position: each branch is a block whose
 * value is its trailing expression. `else if` chains recurse into `rhs`. */
static SnExpr *parse_if_expr(P *p) {
    SnSpan span = cur(p)->span;
    expect(p, SN_TOK_IF);
    SnExpr *e = new_expr(p, SN_EXPR_IF, span);
    int save_nsl = p->no_struct_lit;
    p->no_struct_lit = 1; /* `if x { ... }` — the `{` is the then-block */
    e->lhs = parse_expr(p);
    p->no_struct_lit = save_nsl;
    e->body = parse_block(p);
    if (accept(p, SN_TOK_ELSE)) {
        if (at(p, SN_TOK_IF)) {
            e->rhs = parse_if_expr(p);
        } else {
            e->else_body = parse_block(p);
        }
    }
    return e;
}

static SnExpr *parse_array_lit(P *p, SnSpan span) {
    PCtx ctx = ctx_clear(p);
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
    ctx_restore(p, ctx);
    return e;
}

static SnExpr *parse_literal(P *p, SnExprKind k, SnSpan span) {
    SnExpr *e = new_expr(p, k, span);
    if (k == SN_EXPR_STRING) {
        e->interpolated = cur(p)->has_interpolation;
    }
    e->text = advance_p(p)->text;
    return e;
}

SnExpr *parse_primary(P *p) {
    SnSpan span = cur(p)->span;

    switch (kind(p)) {
    case SN_TOK_INT:     return parse_literal(p, SN_EXPR_INT, span);
    case SN_TOK_LONG:    return parse_literal(p, SN_EXPR_LONG, span);
    case SN_TOK_DOUBLE:  return parse_literal(p, SN_EXPR_DOUBLE, span);
    case SN_TOK_DECIMAL: return parse_literal(p, SN_EXPR_DECIMAL, span);
    case SN_TOK_CHAR:    return parse_literal(p, SN_EXPR_CHAR, span);
    case SN_TOK_STRING:  return parse_literal(p, SN_EXPR_STRING, span);
    case SN_TOK_TRUE:
    case SN_TOK_FALSE:   return parse_literal(p, SN_EXPR_BOOL, span);

    case SN_TOK_THIS:
        advance_p(p);
        return new_expr(p, SN_EXPR_THIS, span);

    case SN_TOK_AWAIT: {
        advance_p(p);
        SnExpr *e = new_expr(p, SN_EXPR_AWAIT, span);
        e->lhs = parse_expr(p);
        return e;
    }
    case SN_TOK_MATCH:
        return parse_match_expr(p);
    case SN_TOK_IF:
        /* Only expression position reaches here — statement `if` is taken by
         * parse_stmt first. `let x = if c { 7 } else { 6 }` lands here. */
        return parse_if_expr(p);
    case SN_TOK_FUNC:
        return parse_anon_fn(p, span);

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
    case SN_TOK_LBRACKET:
        return parse_array_lit(p, span);

    case SN_TOK_LPAREN: {
        if (looks_like_lambda(p)) {
            return parse_lambda(p);
        }
        PCtx ctx = ctx_clear(p);
        advance_p(p);
        SnExpr *inner = parse_expr(p);
        expect(p, SN_TOK_RPAREN);
        ctx_restore(p, ctx);
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

    error_at(p, cur(p), SNOVA_EXPECTED_EXPR,
             "expected an expression, found `%s`", sn_tok_name(kind(p)));
    return NULL;
}
