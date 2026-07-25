/* symbol.h — symbol table with chained (nested) scopes.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §3/§8 step 3.
 *
 * Every `name` accepted here MUST already be an interned pointer (see
 * intern.h) — lookup and collision detection compare names by pointer, never
 * by content. Passing a non-interned string silently breaks lookups (it will
 * define/find nothing, not crash), so callers own that contract.
 *
 * One entry per name per scope. specs/20260719/snovac-p2-resolver-
 * typechecker/llm.md, ambiguity 2 (ratified 2026-07-25): method overloading
 * does not occur anywhere in the measured corpus, so sn_scope_define()
 * rejects a second definition of the same name in the same scope outright
 * instead of keeping a list. If overloading needs to be supported later,
 * that decision reopens this file's shape, not just resolve.c.
 */
#ifndef SNOVAC_SYMBOL_H
#define SNOVAC_SYMBOL_H

#include <stddef.h>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "token.h"

typedef enum {
    SN_SYM_TYPE,    /* class / struct / enum / interface / typealias */
    SN_SYM_FUNC,    /* top-level func */
    SN_SYM_METHOD,  /* method member of a type */
    SN_SYM_FIELD,   /* field member of a type */
    SN_SYM_CONST,   /* const, top-level or member */
    SN_SYM_VARIANT, /* enum variant */
    SN_SYM_PARAM,   /* function / method / lambda parameter */
    SN_SYM_LOCAL,   /* let / var binding introduced inside a body */
    SN_SYM_PACKAGE  /* a package-path segment; decl is NULL (P2.2+) */
} SnSymbolKind;

typedef struct SnSymbol {
    const char *name; /* interned */
    SnSymbolKind kind;
    const SnDecl *decl; /* owning declaration; NULL where kind has none
                          * (SN_SYM_PACKAGE, and SN_SYM_PARAM/LOCAL until a
                          * later pass attaches one) */
    SnSpan span;         /* declaration/binding site, for diagnostics */
    /* Set by check.c (P2.5) for SN_SYM_LOCAL/PARAM, whose type comes from an
     * annotation or inference rather than a `decl` field to read it off of.
     * NULL until check.c defines the symbol; other kinds derive their type
     * from `decl` on demand instead of caching it here. */
    SnTypeRep *value_type;
    /* Set by check.c for SN_SYM_LOCAL (true for `var`, false for `let`).
     * SN_SYM_FIELD mutability instead reads decl->is_mutable directly — no
     * need to duplicate it here. Irrelevant (stays 0) for every other kind. */
    uint8_t is_mutable;
    /* File this symbol was declared in. `span` is only meaningful against it,
     * so anything that emits a diagnostic for a symbol declared elsewhere (a
     * body check walking every package in the graph, say) must point the sink
     * here first. NULL for symbols not created from a file scan — locals,
     * params, and package-path segments. */
    const SnDiagFile *origin;
    struct SnSymbol *next; /* scope bucket chain — not declaration order */
} SnSymbol;

typedef struct SnScope SnScope;
struct SnScope {
    SnScope *parent; /* NULL at the outermost scope */
    SnArena *arena;
    SnSymbol **buckets;
    size_t nbuckets;
    size_t count;
};

void sn_scope_init(SnScope *s, SnArena *a, SnScope *parent);

/* Defines `name` in `s`. Returns NULL without modifying `s` if `name` is
 * already defined directly in `s` — deliberately does NOT check `parent`,
 * since shadowing an outer scope's name is legal (plan.md §7 item 3: local
 * declarations win over anything from an outer scope, including imports,
 * without an error). Callers own emitting the "already declared" diagnostic
 * on a NULL return; this function only detects the collision. */
SnSymbol *sn_scope_define(SnScope *s, const char *name, SnSymbolKind kind,
                           const SnDecl *decl, SnSpan span);

/* Looks up `name` only in `s`, ignoring `parent`. */
SnSymbol *sn_scope_lookup_local(const SnScope *s, const char *name);

/* Looks up `name` in `s`, then walks the `parent` chain outward until found.
 * Returns NULL if no scope in the chain defines it. */
SnSymbol *sn_scope_lookup(const SnScope *s, const char *name);

#endif /* SNOVAC_SYMBOL_H */
