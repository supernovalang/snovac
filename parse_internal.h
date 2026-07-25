/* parse_internal.h — shared state and helpers for the parser family.
 *
 * The parser is split by concern to keep every translation unit small:
 *   parse.c            entry points (sn_parse, sn_parse_expr_only)
 *   parse_type.c       qualified names, types, patterns
 *   parse_expr.c       Pratt machinery: infix/postfix/ternary
 *   parse_primary.c    primaries, lambdas, if/match expressions
 *   parse_stmt.c       statements and blocks
 *   parse_decl.c       declarations
 *   parse_decl_parts.c decorators, parameter lists, type/enum bodies
 *
 * Small helpers live here as static inline so call sites read the same in
 * every file; the recursive-descent workhorses are extern across the family.
 */
#ifndef SNOVAC_PARSE_INTERNAL_H
#define SNOVAC_PARSE_INTERNAL_H

#include "parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Parser band: 0100-0199. 100 and 101 are deliberately skipped here: they are
 * already the project-wide codes for "untyped variable" and "declared/inferred
 * type mismatch" (compiler/src/typeck, builtin/Errors.snova, the Rust Stage 0,
 * and several specs all agree on this), owned by the P2 type checker, not the
 * parser. Found 2026-07-25 while reserving P2's diagnostic range — nothing
 * pinned these two parser codes to 100/101 by value, so moving them here is
 * free; do not reuse 100/101 for anything in snovac/. */
#define SNOVA_EXPECTED_DECL      102
#define SNOVA_EXPECTED_EXPR      103
#define SNOVA_EXPECTED_TYPE      104
#define SNOVA_BAD_ACCESSOR       105
#define SNOVA_EXPECTED_TOKEN     106
#define SNOVA_EXPECTED_NAME      107

typedef struct {
    const SnTokenVec *toks;
    size_t pos;
    SnArena *arena;
    SnDiagSink *diag;
    int errors;
    int panic; /* suppresses cascading diagnostics until we resynchronize */

    /* Context flags. Both are cleared inside any bracketed sub-context
     * (parens, brackets, call arguments, blocks) and restored on exit, so they
     * only constrain the immediate expression level they were set for. */
    int no_struct_lit;   /* condition/scrutinee position: `if x { }` — the `{`
                          * opens a block, never a struct literal */
    int nl_stops_infix;  /* match-arm body: an infix operator that starts a new
                          * line begins the next arm's pattern (`-1 -> ...`),
                          * not a continuation of this body */
} P;

typedef struct {
    int no_struct_lit;
    int nl_stops_infix;
} PCtx;

/* ── token access ─────────────────────────────────────────────────────────── */

static inline const SnToken *cur(P *p) { return &p->toks->data[p->pos]; }

static inline SnTokKind kind(P *p) { return cur(p)->kind; }

static inline const SnToken *peek_at(P *p, size_t n) {
    size_t i = p->pos + n;
    if (i >= p->toks->len) {
        i = p->toks->len - 1; /* EOF */
    }
    return &p->toks->data[i];
}

static inline int at(P *p, SnTokKind k) { return kind(p) == k; }
static inline int at_end_p(P *p) { return kind(p) == SN_TOK_EOF; }

static inline const SnToken *advance_p(P *p) {
    const SnToken *t = cur(p);
    if (!at_end_p(p)) {
        p->pos++;
    }
    return t;
}

static inline int accept(P *p, SnTokKind k) {
    if (at(p, k)) {
        advance_p(p);
        return 1;
    }
    return 0;
}

static inline int at_name_tok(SnTokKind k) {
    return k == SN_TOK_IDENT || sn_tok_is_keyword(k) || k == SN_TOK_UNDERSCORE;
}

static inline int at_name(P *p) {
    return at(p, SN_TOK_IDENT) || sn_tok_is_keyword(kind(p)) ||
           at(p, SN_TOK_UNDERSCORE);
}

/* ── diagnostics ──────────────────────────────────────────────────────────── */

static inline void error_at(P *p, const SnToken *t, int code,
                            const char *fmt, ...) {
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

static inline int expect(P *p, SnTokKind k) {
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
static inline const char *expect_name(P *p) {
    if (at_name(p)) {
        return advance_p(p)->text;
    }
    error_at(p, cur(p), SNOVA_EXPECTED_NAME, "expected a name, found `%s`",
             sn_tok_name(kind(p)));
    return "<error>";
}

/* ── allocation ───────────────────────────────────────────────────────────── */

static inline SnExpr *new_expr(P *p, SnExprKind k, SnSpan span) {
    SnExpr *e = (SnExpr *)sn_arena_calloc(p->arena, sizeof(SnExpr));
    e->kind = k;
    e->span = span;
    return e;
}

static inline SnStmt *new_stmt(P *p, SnStmtKind k, SnSpan span) {
    SnStmt *s = (SnStmt *)sn_arena_calloc(p->arena, sizeof(SnStmt));
    s->kind = k;
    s->span = span;
    return s;
}

static inline SnDecl *new_decl(P *p, SnDeclKind k, SnSpan span) {
    SnDecl *d = (SnDecl *)sn_arena_calloc(p->arena, sizeof(SnDecl));
    d->kind = k;
    d->span = span;
    return d;
}

/* ── context flags ────────────────────────────────────────────────────────── */

/* Enter a bracketed sub-context: inner expressions are unconstrained. */
static inline PCtx ctx_clear(P *p) {
    PCtx c = {p->no_struct_lit, p->nl_stops_infix};
    p->no_struct_lit = 0;
    p->nl_stops_infix = 0;
    return c;
}

static inline void ctx_restore(P *p, PCtx c) {
    p->no_struct_lit = c.no_struct_lit;
    p->nl_stops_infix = c.nl_stops_infix;
}

/* ── cross-file workhorses ────────────────────────────────────────────────── */

/* parse_type.c */
const char *parse_qualified(P *p, SnSpan *span_out);
void parse_type_args(P *p, SnList *out);
SnType *parse_type(P *p);
SnPattern *parse_pattern(P *p);

/* parse_expr.c */
SnExpr *parse_expr(P *p);

/* parse_primary.c */
SnExpr *parse_primary(P *p);
void parse_match_arms(P *p, SnList *out);

/* parse_stmt.c */
SnStmt *parse_stmt(P *p);
SnStmt *parse_block(P *p);

/* parse_decl.c */
SnDecl *parse_decl(P *p, int in_type_body);

/* parse_decl_parts.c */
void parse_decorators(P *p, SnList *out);
void parse_generic_params(P *p, SnList *out);
void parse_params(P *p, SnList *out);
void parse_accessor_block(P *p, SnDecl *d);
void parse_type_body(P *p, SnDecl *d);
void parse_enum_body(P *p, SnDecl *d);

#endif /* SNOVAC_PARSE_INTERNAL_H */
