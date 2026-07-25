/* parse.c — parser entry points and the compilation-unit loop.
 *
 * The recursive-descent body is split across parse_type.c, parse_expr.c,
 * parse_primary.c, parse_stmt.c, parse_decl.c and parse_decl_parts.c; the
 * shared state and helpers live in parse_internal.h.
 */
#include "parse_internal.h"

static void parser_init(P *p, SnArena *arena, SnDiagSink *diag,
                        const SnTokenVec *toks) {
    p->toks = toks;
    p->pos = 0;
    p->arena = arena;
    p->diag = diag;
    p->errors = 0;
    p->panic = 0;
    p->no_struct_lit = 0;
    p->nl_stops_infix = 0;
}

SnExpr *sn_parse_expr_only(SnArena *arena, SnDiagSink *diag,
                           const SnTokenVec *toks) {
    P p;
    parser_init(&p, arena, diag, toks);
    SnExpr *e = parse_expr(&p);
    return p.errors ? NULL : e;
}

/* Skip to something that can begin a top-level declaration. */
static void resync_top_level(P *p) {
    while (!at_end_p(p) && !at(p, SN_TOK_CLASS) && !at(p, SN_TOK_STRUCT) &&
           !at(p, SN_TOK_ENUM) && !at(p, SN_TOK_INTERFACE) &&
           !at(p, SN_TOK_FUNC) && !at(p, SN_TOK_AT) && !at(p, SN_TOK_PUBLIC) &&
           !at(p, SN_TOK_PRIVATE)) {
        advance_p(p);
    }
    p->panic = 0;
}

int sn_parse(SnArena *arena, SnDiagSink *diag, const SnTokenVec *toks,
             SnUnit *out) {
    P p;
    parser_init(&p, arena, diag, toks);
    memset(out, 0, sizeof(*out));

    if (at(&p, SN_TOK_PACKAGE)) {
        out->package_span = cur(&p)->span;
        advance_p(&p);
        out->package = parse_qualified(&p, NULL);
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
            resync_top_level(&p);
        }
    }

    return p.errors ? 1 : 0;
}
