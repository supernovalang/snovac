/* parse_decl.c — declarations: modifiers, the by-text declaration forms
 * (`type`, `constructor`, `extension`) and the keyword-led ones. */
#include "parse_internal.h"

typedef struct {
    SnVisibility vis;
    uint8_t is_static;
    uint8_t is_override;
    uint8_t is_async;
} Modifiers;

/* Is `w` a soft modifier — a word that only acts as one when a declaration
 * keyword follows, so it stays an ordinary identifier everywhere else? */
static int is_soft_modifier(const char *w) {
    return strcmp(w, "data") == 0 || strcmp(w, "unsafe") == 0 ||
           strcmp(w, "sealed") == 0 || strcmp(w, "abstract") == 0 ||
           strcmp(w, "final") == 0 || strcmp(w, "open") == 0 ||
           strcmp(w, "internal") == 0;
}

static int starts_decl_kw(SnTokKind k) {
    return k == SN_TOK_CLASS || k == SN_TOK_STRUCT || k == SN_TOK_ENUM ||
           k == SN_TOK_INTERFACE || k == SN_TOK_METHOD || k == SN_TOK_FUNC ||
           k == SN_TOK_LET || k == SN_TOK_VAR;
}

static Modifiers parse_modifiers(P *p) {
    Modifiers m = {SN_VIS_DEFAULT, 0, 0, 0};
    for (;;) {
        if (at(p, SN_TOK_PUBLIC))    { m.vis = SN_VIS_PUBLIC;    advance_p(p); continue; }
        if (at(p, SN_TOK_PRIVATE))   { m.vis = SN_VIS_PRIVATE;   advance_p(p); continue; }
        if (at(p, SN_TOK_PROTECTED)) { m.vis = SN_VIS_PROTECTED; advance_p(p); continue; }
        if (at(p, SN_TOK_STATIC))    { m.is_static = 1;   advance_p(p); continue; }
        if (at(p, SN_TOK_OVERRIDE))  { m.is_override = 1; advance_p(p); continue; }
        /* `async` is a modifier only when a declaration keyword follows;
         * otherwise it is an ordinary identifier. */
        if (at(p, SN_TOK_ASYNC) && (peek_at(p, 1)->kind == SN_TOK_METHOD ||
                                    peek_at(p, 1)->kind == SN_TOK_FUNC)) {
            m.is_async = 1;
            advance_p(p);
            continue;
        }
        /* `pulsar func worker(): unit { }` — pulsar is a declaration modifier
         * here, and a statement keyword elsewhere. */
        if (at(p, SN_TOK_PULSAR) && (peek_at(p, 1)->kind == SN_TOK_FUNC ||
                                     peek_at(p, 1)->kind == SN_TOK_METHOD)) {
            advance_p(p);
            continue;
        }
        if (at(p, SN_TOK_IDENT) && is_soft_modifier(cur(p)->text) &&
            starts_decl_kw(peek_at(p, 1)->kind)) {
            advance_p(p);
            continue;
        }
        break;
    }
    return m;
}

/* Supertypes written after `:`, `extends` or `implements`. The latter two are
 * not lexer keywords — recognised by text, so they stay usable as identifiers.
 * The parser does not tell a base class from an interface: that needs symbol
 * resolution, which is P2's job. */
static void parse_supertypes(P *p, SnDecl *d, int allow_keywords) {
    for (;;) {
        int is_kw = allow_keywords && at(p, SN_TOK_IDENT) &&
                    (strcmp(cur(p)->text, "extends") == 0 ||
                     strcmp(cur(p)->text, "implements") == 0) &&
                    at_name_tok(peek_at(p, 1)->kind);
        if (!is_kw && !accept(p, SN_TOK_COLON)) {
            break;
        }
        if (is_kw) {
            advance_p(p);
        }
        for (;;) {
            SnType *sup = parse_type(p);
            if (sup) {
                sn_list_push(p->arena, &d->supertypes, sup);
            }
            if (!accept(p, SN_TOK_COMMA)) {
                break;
            }
        }
        if (!allow_keywords) {
            break;
        }
    }
}

/* A `data class` body declares fields as bare `name: Type` with no `let`/`var`.
 * Recognised only inside a type body, so a stray identifier at top level still
 * reports a missing declaration. */
static SnDecl *parse_bare_field(P *p, SnSpan span) {
    SnDecl *f = new_decl(p, SN_DECL_FIELD, span);
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

/* `constructor(description: string) { ... }` — a member whose name is fixed by
 * the by-text keyword `constructor`. Parsed as a method of that name; P2 owns
 * its special dispatch. */
static SnDecl *parse_constructor(P *p, SnSpan span) {
    SnDecl *c = new_decl(p, SN_DECL_METHOD, span);
    c->name = advance_p(p)->text;
    parse_params(p, &c->params);
    if (accept(p, SN_TOK_COLON)) {
        c->ret = parse_type(p);
    }
    if (at(p, SN_TOK_LBRACE)) {
        c->body = parse_block(p);
    }
    return c;
}

/* `extension Counter { func isZero(): bool = value == 0 }` — adds members to
 * an existing type. Top level only, recognised by text. */
static SnDecl *parse_extension(P *p, SnSpan span) {
    advance_p(p);
    SnDecl *x = new_decl(p, SN_DECL_EXTENSION, span);
    x->name = expect_name(p);
    if (at(p, SN_TOK_LT)) {
        parse_generic_params(p, &x->generics);
    }
    parse_type_body(p, x);
    return x;
}

/* `public type RowDecoder<T> = func(SqlRow) -> Result<T, E>`. `type` is not a
 * lexer keyword — it is a common identifier — so it is recognised by text plus
 * the shape that follows. */
static SnDecl *parse_type_alias(P *p, SnSpan span) {
    advance_p(p);
    SnDecl *ta = new_decl(p, SN_DECL_TYPEALIAS, span);
    ta->name = expect_name(p);
    if (at(p, SN_TOK_LT)) {
        parse_generic_params(p, &ta->generics);
    }
    expect(p, SN_TOK_ASSIGN);
    ta->type = parse_type(p);
    accept(p, SN_TOK_SEMI);
    return ta;
}

static void parse_routine_rest(P *p, SnDecl *d) {
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
        return;
    }
    if (accept(p, SN_TOK_ASSIGN)) {
        /* Expression body: `func main(): int = 0`. The right-hand side may
         * itself be a block (`method foo(): int = { ... }`). */
        if (at(p, SN_TOK_LBRACE)) {
            d->body = parse_block(p);
            return;
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
}

static int decl_kind_of(SnTokKind k, SnDeclKind *out) {
    switch (k) {
    case SN_TOK_CLASS:     *out = SN_DECL_CLASS;     return 1;
    case SN_TOK_STRUCT:    *out = SN_DECL_STRUCT;    return 1;
    case SN_TOK_ENUM:      *out = SN_DECL_ENUM;      return 1;
    case SN_TOK_INTERFACE: *out = SN_DECL_INTERFACE; return 1;
    case SN_TOK_METHOD:    *out = SN_DECL_METHOD;    return 1;
    case SN_TOK_FUNC:      *out = SN_DECL_FUNC;      return 1;
    case SN_TOK_LET:
    case SN_TOK_VAR:       *out = SN_DECL_FIELD;     return 1;
    case SN_TOK_CONST:     *out = SN_DECL_CONST;     return 1;
    default: return 0;
    }
}

static SnDecl *parse_by_text_decl(P *p, int in_type_body, SnSpan span) {
    if (in_type_body && at_name(p) && peek_at(p, 1)->kind == SN_TOK_COLON &&
        !at(p, SN_TOK_METHOD) && !at(p, SN_TOK_FUNC)) {
        return parse_bare_field(p, span);
    }
    if (!at(p, SN_TOK_IDENT)) {
        return NULL;
    }
    const char *w = cur(p)->text;
    if (in_type_body && strcmp(w, "constructor") == 0 &&
        peek_at(p, 1)->kind == SN_TOK_LPAREN) {
        return parse_constructor(p, span);
    }
    if (!in_type_body && strcmp(w, "extension") == 0 &&
        at_name_tok(peek_at(p, 1)->kind)) {
        return parse_extension(p, span);
    }
    if (strcmp(w, "type") == 0 && at_name_tok(peek_at(p, 1)->kind)) {
        return parse_type_alias(p, span);
    }
    return NULL;
}

SnDecl *parse_decl(P *p, int in_type_body) {
    SnList decorators = {0};
    parse_decorators(p, &decorators);

    SnSpan span = cur(p)->span;
    Modifiers mods = parse_modifiers(p);

    SnDecl *by_text = parse_by_text_decl(p, in_type_body, span);
    if (by_text) {
        by_text->decorators = decorators;
        by_text->vis = mods.vis;
        by_text->is_static = mods.is_static;
        return by_text;
    }

    SnDeclKind k;
    if (!decl_kind_of(kind(p), &k)) {
        error_at(p, cur(p), SNOVA_EXPECTED_DECL,
                 "expected a declaration, found `%s`", sn_tok_name(kind(p)));
        return NULL;
    }

    SnDecl *d = new_decl(p, k, span);
    d->decorators = decorators;
    d->vis = mods.vis;
    d->is_static = mods.is_static;
    d->is_override = mods.is_override;
    d->is_async = mods.is_async;

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
        /* `class Foo(a: int)` primary-constructor form is read and kept only
         * as shape until P2. */
        if (at(p, SN_TOK_LPAREN)) {
            parse_params(p, &d->params);
        }
        parse_supertypes(p, d, 1);
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
        parse_supertypes(p, d, 0); /* `enum MathError : Error { ... }` */
        parse_enum_body(p, d);
        return d;

    case SN_DECL_METHOD:
    case SN_DECL_FUNC:
        parse_routine_rest(p, d);
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
    return d;
}
