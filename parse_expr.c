/* parse_expr.c — the Pratt engine: postfix chains, infix binding powers and
 * the ternary/error-propagation split. Primaries live in parse_primary.c. */
#include "parse_internal.h"

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
    case SN_TOK_QQ:      b.lbp = 2; b.right_assoc = 1; break; /* `a ?? b` right-associative null-coalescing */
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
    /* Not SN_TOK_LT/SN_TOK_GT: those already mean "less/greater than" to
     * every consumer of e->op. A dedicated op keeps shift from being
     * silently misread as a comparison. */
    *op_out = (k == SN_TOK_LT) ? SN_TOK_SHL : SN_TOK_SHR;
    return 1;
}

static void parse_call_args(P *p, SnExpr *call) {
    PCtx ctx = ctx_clear(p);
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
    ctx_restore(p, ctx);
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

/* Struct literal: `UserDto { id: "1" }`, `ProductRepository {}`.
 * Only after a type-shaped expression, never in condition or scrutinee
 * position (no_struct_lit), and only when the braces have literal shape —
 * empty, or `name:` right away — so a block following an identifier cannot be
 * misread as a literal. */
static int at_struct_lit(P *p, const SnExpr *lhs) {
    if (!at(p, SN_TOK_LBRACE) || p->no_struct_lit) {
        return 0;
    }
    if (lhs->kind != SN_EXPR_IDENT && lhs->kind != SN_EXPR_MEMBER) {
        return 0;
    }
    SnTokKind n1 = peek_at(p, 1)->kind;
    return n1 == SN_TOK_RBRACE ||
           (at_name_tok(n1) && peek_at(p, 2)->kind == SN_TOK_COLON);
}

static SnExpr *parse_struct_lit(P *p, SnExpr *lhs, SnSpan span) {
    PCtx ctx = ctx_clear(p);
    advance_p(p);
    SnExpr *e = new_expr(p, SN_EXPR_STRUCT_LIT, span);
    e->lhs = lhs;
    while (!at(p, SN_TOK_RBRACE) && !at_end_p(p)) {
        const char *fname = expect_name(p);
        expect(p, SN_TOK_COLON);
        SnExpr *fval = parse_expr(p);
        sn_list_push(p->arena, &e->field_names, (void *)fname);
        sn_list_push(p->arena, &e->args, fval);
        if (!accept(p, SN_TOK_COMMA)) {
            break;
        }
    }
    expect(p, SN_TOK_RBRACE);
    ctx_restore(p, ctx);
    return e;
}

/* Disambiguates `cond ? then_e : else_e` (ternary) from `expr?` (postfix error-propagation).
 * A ternary always has a matching `:` at the current expression nesting depth downstream. */
static int is_ternary_question(P *p) {
    int depth = 0;
    size_t i = 1;
    for (;;) {
        const SnToken *tok = peek_at(p, i);
        SnTokKind k = tok->kind;

        if (k == SN_TOK_EOF) {
            return 0;
        }

        if (depth == 0) {
            if (k == SN_TOK_COLON) {
                return 1;
            }
            if (k == SN_TOK_SEMI || k == SN_TOK_COMMA ||
                k == SN_TOK_RPAREN || k == SN_TOK_RBRACE || k == SN_TOK_RBRACKET ||
                k == SN_TOK_FATARROW) {
                return 0;
            }
            if (sn_tok_is_keyword(k)) {
                switch (k) {
                case SN_TOK_AS: case SN_TOK_IS: case SN_TOK_TRUE: case SN_TOK_FALSE:
                case SN_TOK_THIS: case SN_TOK_NEW: case SN_TOK_AWAIT:
                    break;
                default:
                    return 0;
                }
            }
        }

        if (k == SN_TOK_LPAREN || k == SN_TOK_LBRACE || k == SN_TOK_LBRACKET) {
            depth++;
        } else if (k == SN_TOK_RPAREN || k == SN_TOK_RBRACE || k == SN_TOK_RBRACKET) {
            depth--;
            if (depth < 0) {
                return 0;
            }
        }

        i++;
    }
}

static SnExpr *parse_postfix(P *p, SnExpr *lhs) {
    for (;;) {
        if (!lhs) {
            return NULL;
        }
        SnSpan span = cur(p)->span;

        if (at(p, SN_TOK_DOT) || at(p, SN_TOK_QDOT)) {
            SnTokKind op = kind(p);
            advance_p(p);
            SnExpr *e = new_expr(p, SN_EXPR_MEMBER, span);
            e->op = op;
            e->lhs = lhs;
            e->text = expect_name(p); /* keywords are legal member names */
            lhs = e;
            continue;
        }
        if (at(p, SN_TOK_COLONCOLON)) {
            /* `::` is only allowed for package function calls: `SomePackage::functionName(...)`. */
            SnTokKind n1 = peek_at(p, 1)->kind;
            SnTokKind n2 = peek_at(p, 2)->kind;
            if (at_name_tok(n1) && n2 == SN_TOK_LPAREN) {
                advance_p(p);
                SnExpr *e = new_expr(p, SN_EXPR_MEMBER, span);
                e->op = SN_TOK_COLONCOLON;
                e->lhs = lhs;
                e->text = expect_name(p);
                lhs = e;
                continue;
            } else {
                error_at(p, cur(p), SNOVA_EXPECTED_TOKEN, "`::` is only allowed for package function calls like `SomePackage::functionName(...)`");
                advance_p(p);
                continue;
            }
        }
        if (at(p, SN_TOK_QUESTION) && !is_ternary_question(p)) {
            advance_p(p);
            SnExpr *u = new_expr(p, SN_EXPR_UNARY, span);
            u->op = SN_TOK_QUESTION;
            u->lhs = lhs;
            lhs = u;
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
            PCtx ctx = ctx_clear(p);
            advance_p(p);
            SnExpr *e = new_expr(p, SN_EXPR_INDEX, span);
            e->lhs = lhs;
            e->rhs = parse_expr(p);
            expect(p, SN_TOK_RBRACKET);
            ctx_restore(p, ctx);
            lhs = e;
            continue;
        }
        if (at_struct_lit(p, lhs)) {
            lhs = parse_struct_lit(p, lhs, span);
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
        /* In a match-arm body, an infix operator that opens a new line is not a
         * continuation: `0 -> return 1` followed by `-1 -> ...` must end the
         * body at the newline so `-1` reads as the next arm's pattern. */
        if (p->nl_stops_infix && p->pos > 0 &&
            cur(p)->span.line > p->toks->data[p->pos - 1].span.line) {
            break;
        }
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

SnExpr *parse_expr(P *p) {
    SnExpr *e = parse_binary(p, 1);

    if (at(p, SN_TOK_QUESTION) && is_ternary_question(p)) {
        SnSpan span = cur(p)->span;
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
    return e;
}
