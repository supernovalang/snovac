/* parse_stmt.c — blocks and statements. */
#include "parse_internal.h"

SnStmt *parse_block(P *p) {
    SnSpan span = cur(p)->span;
    SnStmt *s = new_stmt(p, SN_STMT_BLOCK, span);
    PCtx ctx = ctx_clear(p);
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
    /* The while loop above only exits at `}` or end of file (its own
     * condition, and the panic-resync loop inside it, both stop there) — so
     * reaching EOF here specifically means the `{` opened above never found
     * its match. That is SNOVA008 ("unclosed `{` delimiter",
     * docs/snovalang-diagnostics.md), not the generic SNOVA_EXPECTED_TOKEN
     * every other `expect()` call in this file falls back to. */
    if (at(p, SN_TOK_RBRACE)) {
        advance_p(p);
    } else {
        error_at(p, cur(p), SNOVA_UNCLOSED_BRACE,
                "unclosed `{` — reached end of file before finding a matching `}`");
    }
    ctx_restore(p, ctx);
    return s;
}

/* Body of a control-flow construct: a block, or a single statement. */
static SnStmt *parse_body(P *p) {
    return at(p, SN_TOK_LBRACE) ? parse_block(p) : parse_stmt(p);
}

/* Condition or iterable position: the `{` that follows opens the body, so a
 * struct literal must not be read there. */
static SnExpr *parse_headless_expr(P *p) {
    p->no_struct_lit = 1;
    SnExpr *e = parse_expr(p);
    p->no_struct_lit = 0;
    return e;
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

static SnStmt *parse_for(P *p, SnSpan span) {
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
    s->expr = paren ? parse_expr(p) : parse_headless_expr(p);
    if (paren) {
        expect(p, SN_TOK_RPAREN);
    }
    s->then_br = parse_body(p);
    return s;
}

static SnStmt *parse_try(P *p, SnSpan span) {
    advance_p(p);
    SnStmt *s = new_stmt(p, SN_STMT_TRY, span);
    s->then_br = parse_body(p);
    while (at(p, SN_TOK_CATCH)) {
        advance_p(p);
        int paren = accept(p, SN_TOK_LPAREN);
        const char *nm = NULL;
        if (at_name(p)) {
            nm = expect_name(p);
            if (accept(p, SN_TOK_COLON)) {
                parse_type(p);
            }
        }
        if (paren) {
            expect(p, SN_TOK_RPAREN);
        }
        SnStmt *c = parse_block(p);
        c->name = nm;
        sn_list_push(p->arena, &s->catches, c);
    }
    /* `finally` is not a lexer keyword — it is an ordinary identifier
     * everywhere else — so the clause is recognised by text plus shape. */
    if (at(p, SN_TOK_IDENT) && strcmp(cur(p)->text, "finally") == 0 &&
        peek_at(p, 1)->kind == SN_TOK_LBRACE) {
        advance_p(p);
        s->finally_br = parse_block(p);
    }
    return s;
}

/* Receive-bind: `x <~ expr`, `ok, value <~ ch.tryReceive()`. Declares mutable
 * bindings with inferred types, and receives from a channel when the
 * right-hand side is a Channel<T> — the two readings differ only by type, so
 * the parser emits SN_STMT_VAR either way and leaves the choice to P2.
 * Detected by scanning `name (, name)*` up to a `<~`, which keeps a plain
 * expression statement starting with an identifier untouched. */
static int at_receive_bind(P *p) {
    if (!at_name(p)) {
        return 0;
    }
    size_t i = p->pos;
    for (;;) {
        if (!at_name_tok(p->toks->data[i].kind)) {
            return 0;
        }
        i++;
        if (p->toks->data[i].kind == SN_TOK_COMMA) {
            i++;
            continue;
        }
        break;
    }
    return p->toks->data[i].kind == SN_TOK_RECV_BIND;
}

static SnStmt *parse_receive_bind(P *p, SnSpan span) {
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

SnStmt *parse_stmt(P *p) {
    SnSpan span = cur(p)->span;

    switch (kind(p)) {
    case SN_TOK_LET: return parse_binding(p, SN_STMT_LET);
    case SN_TOK_VAR: return parse_binding(p, SN_STMT_VAR);
    case SN_TOK_LBRACE: return parse_block(p);
    case SN_TOK_FOR: return parse_for(p, span);
    case SN_TOK_TRY: return parse_try(p, span);

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
         * `if (isGet || isPost) && selected(x) {` the parens are a
         * sub-expression, not a wrapper, and consuming them here strands the
         * `&&`. parse_expr already parses `(...)` as a primary, so the fully
         * parenthesised form `if (cond) {` falls out of the same path. */
        s->expr = parse_headless_expr(p);
        s->then_br = parse_body(p);
        if (accept(p, SN_TOK_ELSE)) {
            s->else_br = parse_body(p);
        }
        return s;
    }
    case SN_TOK_WHILE: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_WHILE, span);
        s->expr = parse_headless_expr(p); /* same reasoning as `if` above */
        s->then_br = parse_body(p);
        return s;
    }
    case SN_TOK_MATCH: {
        advance_p(p);
        SnStmt *s = new_stmt(p, SN_STMT_MATCH, span);
        s->expr = parse_headless_expr(p); /* `{` opens the arm list */
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
        s->then_br = parse_body(p);
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
    default:
        break;
    }

    if (at_receive_bind(p)) {
        return parse_receive_bind(p, span);
    }

    SnStmt *s = new_stmt(p, SN_STMT_EXPR, span);
    s->expr = parse_expr(p);
    accept(p, SN_TOK_SEMI);
    return s;
}
