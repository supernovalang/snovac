/* intern.h — string interning.
 *
 * Deferred in P0, now a P2 prerequisite (specs/20260719/snovac-p2-resolver-
 * typechecker/plan.md §3): the resolver and type checker compare names by
 * pointer, never by content, so every name that reaches symbol.c or types.c
 * must first pass through here.
 *
 * Token text produced by lex.c/parse.c is NOT interned today — it is a fresh
 * sn_arena_strndup() per token, so two occurrences of the same identifier
 * currently get different pointers. Callers (resolve.c and later) must call
 * sn_intern_cstr() on every AST-provided name before using it with symbol.c
 * or comparing it against another interned name. Interning the lexer's own
 * token text retroactively is a P1 front-end change, not part of P2 — it is
 * unnecessary anyway, since routing names through the intern table at the
 * point of use (declaration collection, name resolution) gives the same
 * pointer-equality guarantee without touching already-gated P1 files.
 */
#ifndef SNOVAC_INTERN_H
#define SNOVAC_INTERN_H

#include <stddef.h>

#include "arena.h"

typedef struct SnInternEntry SnInternEntry;

typedef struct {
    SnArena *arena;
    SnInternEntry **buckets; /* arena-allocated, power-of-two length */
    size_t nbuckets;
    size_t count;
} SnInternTable;

void sn_intern_init(SnInternTable *t, SnArena *a);

/* Returns the canonical, NUL-terminated pointer for the byte string [s, s+len).
 * Two calls with equal content always return the same pointer, so identity
 * comparison (`==`) is a valid content comparison for any string that passed
 * through here. */
const char *sn_intern(SnInternTable *t, const char *s, size_t len);
const char *sn_intern_cstr(SnInternTable *t, const char *s);

#endif /* SNOVAC_INTERN_H */
