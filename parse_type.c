/* parse_type.c — qualified names, type references and patterns. */
#include "parse_internal.h"

/* Dotted name: a.b.C — joined into one interned string. */
const char *parse_qualified(P *p, SnSpan *span_out) {
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
void parse_type_args(P *p, SnList *out) {
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

SnType *parse_type(P *p) {
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
        if (accept(p, SN_TOK_TILDE_ARROW) || accept(p, SN_TOK_ARROW)) {
            t->ret = parse_type(p);
            return wrap_array_suffix(p, t);
        }
        /* No arrow: `(K, V)` is a tuple type (`Option<(K, V)>`), and a single
         * parenthesised type is just grouping. `()` with no arrow stays an
         * error — reported as the missing arrow of a function type. */
        if (t->params.len == 1) {
            return wrap_array_suffix(p, (SnType *)t->params.items[0]);
        }
        if (t->params.len >= 2) {
            t->kind = SN_TYPE_TUPLE;
            return wrap_array_suffix(p, t);
        }
        expect(p, SN_TOK_ARROW);
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
    /* `T?` optional shorthand, recorded as is_optional on the SnType node. */
    if (at(p, SN_TOK_QQ)) {
        error_at(p, cur(p), SNOVA_NESTED_OPTIONAL, "nested optional type `T??` is not allowed");
        advance_p(p);
        t->is_optional = 1;
    } else if (accept(p, SN_TOK_QUESTION)) {
        t->is_optional = 1;
        if (at(p, SN_TOK_QUESTION)) {
            error_at(p, cur(p), SNOVA_NESTED_OPTIONAL, "nested optional type `T??` is not allowed");
            advance_p(p);
        }
    }
    return wrap_array_suffix(p, t);
}

/* ── patterns ─────────────────────────────────────────────────────────────── */

SnPattern *parse_pattern(P *p) {
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
