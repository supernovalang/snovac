/* parse_decl_parts.c — the pieces a declaration is built from: decorators,
 * generic parameters, parameter lists, and type/enum bodies. */
#include "parse_internal.h"

/* One decorator argument. `description: "..."` and `description = "..."` are
 * both used in the corpus, and `returnType: List<T>` passes a bare TYPE rather
 * than a value — so the type reading is tried first when the shape allows, and
 * rewound when it does not pan out. */
static void parse_decorator_arg(P *p, SnDecorator *d) {
    const char *argname = NULL;
    if (at_name(p) && (peek_at(p, 1)->kind == SN_TOK_COLON ||
                       peek_at(p, 1)->kind == SN_TOK_ASSIGN)) {
        argname = cur(p)->text;
        advance_p(p);
        advance_p(p);
    }

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
}

void parse_decorators(P *p, SnList *out) {
    while (at(p, SN_TOK_AT)) {
        SnSpan span = cur(p)->span;
        advance_p(p);
        SnDecorator *d =
            (SnDecorator *)sn_arena_calloc(p->arena, sizeof(SnDecorator));
        d->span = span;
        d->name = parse_qualified(p, NULL);
        if (at(p, SN_TOK_LT)) { /* @SqlQuery<UserRow>(...) */
            SnList dargs = {0};
            parse_type_args(p, &dargs);
        }

        if (accept(p, SN_TOK_LPAREN)) {
            if (!at(p, SN_TOK_RPAREN)) {
                for (;;) {
                    parse_decorator_arg(p, d);
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

void parse_generic_params(P *p, SnList *out) {
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

void parse_params(P *p, SnList *out) {
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

void parse_type_body(P *p, SnDecl *d) {
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

void parse_enum_body(P *p, SnDecl *d) {
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
            while (!at_end_p(p) && !at(p, SN_TOK_RBRACE) &&
                   !at(p, SN_TOK_COMMA)) {
                advance_p(p);
            }
            accept(p, SN_TOK_COMMA);
            p->panic = 0;
        }
    }
    expect(p, SN_TOK_RBRACE);
}
