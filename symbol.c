#include "symbol.h"

#include <stdint.h>

#define SN_SCOPE_INITIAL_BUCKETS 8u
#define SN_SCOPE_MAX_LOAD_NUM 3u
#define SN_SCOPE_MAX_LOAD_DEN 4u

/* Names are interned, so identity is the whole comparison; hashing the
 * pointer value itself (Fibonacci multiplicative hash) avoids re-hashing
 * string content on every lookup. */
static size_t hash_ptr(const char *name, size_t nbuckets) {
    uint64_t p = (uint64_t)(uintptr_t)(const void *)name;
    uint64_t h = p * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return (size_t)(h & (uint64_t)(nbuckets - 1u));
}

void sn_scope_init(SnScope *s, SnArena *a, SnScope *parent) {
    s->parent = parent;
    s->arena = a;
    s->nbuckets = SN_SCOPE_INITIAL_BUCKETS;
    s->buckets =
        (SnSymbol **)sn_arena_calloc(a, s->nbuckets * sizeof(SnSymbol *));
    s->count = 0;
}

static void rehash(SnScope *s) {
    size_t new_n = s->nbuckets * 2u;
    SnSymbol **nb =
        (SnSymbol **)sn_arena_calloc(s->arena, new_n * sizeof(SnSymbol *));
    for (size_t i = 0; i < s->nbuckets; i++) {
        SnSymbol *sym = s->buckets[i];
        while (sym) {
            SnSymbol *next = sym->next;
            size_t idx = hash_ptr(sym->name, new_n);
            sym->next = nb[idx];
            nb[idx] = sym;
            sym = next;
        }
    }
    s->buckets = nb;
    s->nbuckets = new_n;
}

SnSymbol *sn_scope_lookup_local(const SnScope *s, const char *name) {
    size_t idx = hash_ptr(name, s->nbuckets);
    for (SnSymbol *sym = s->buckets[idx]; sym; sym = sym->next) {
        if (sym->name == name) {
            return sym;
        }
    }
    return NULL;
}

SnSymbol *sn_scope_lookup(const SnScope *s, const char *name) {
    for (const SnScope *cur = s; cur; cur = cur->parent) {
        SnSymbol *found = sn_scope_lookup_local(cur, name);
        if (found) {
            return found;
        }
    }
    return NULL;
}

SnSymbol *sn_scope_define(SnScope *s, const char *name, SnSymbolKind kind,
                           const SnDecl *decl, SnSpan span) {
    if (sn_scope_lookup_local(s, name)) {
        return NULL;
    }

    if ((s->count + 1u) * SN_SCOPE_MAX_LOAD_DEN >
        s->nbuckets * SN_SCOPE_MAX_LOAD_NUM) {
        rehash(s);
    }

    SnSymbol *sym = (SnSymbol *)sn_arena_calloc(s->arena, sizeof(SnSymbol));
    sym->name = name;
    sym->kind = kind;
    sym->decl = decl;
    sym->span = span;

    size_t idx = hash_ptr(name, s->nbuckets);
    sym->next = s->buckets[idx];
    s->buckets[idx] = sym;
    s->count++;
    return sym;
}
