#include "intern.h"

#include <stdint.h>
#include <string.h>

#define SN_INTERN_INITIAL_BUCKETS 64u
#define SN_INTERN_MAX_LOAD_NUM 3u
#define SN_INTERN_MAX_LOAD_DEN 4u

struct SnInternEntry {
    SnInternEntry *next;
    uint64_t hash;
    size_t len;
    char text[]; /* NUL-terminated, arena-allocated with the entry */
};

/* FNV-1a. Simple, deterministic, and good enough for identifier-sized keys —
 * this is a compiler's symbol table, not a hash-flooding-hostile service. */
static uint64_t fnv1a(const char *s, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void sn_intern_init(SnInternTable *t, SnArena *a) {
    t->arena = a;
    t->nbuckets = SN_INTERN_INITIAL_BUCKETS;
    t->buckets =
        (SnInternEntry **)sn_arena_calloc(a, t->nbuckets * sizeof(SnInternEntry *));
    t->count = 0;
}

static size_t bucket_of(uint64_t hash, size_t nbuckets) {
    return (size_t)(hash & (uint64_t)(nbuckets - 1u));
}

static void rehash(SnInternTable *t) {
    size_t new_n = t->nbuckets * 2u;
    SnInternEntry **nb =
        (SnInternEntry **)sn_arena_calloc(t->arena, new_n * sizeof(SnInternEntry *));
    for (size_t i = 0; i < t->nbuckets; i++) {
        SnInternEntry *e = t->buckets[i];
        while (e) {
            SnInternEntry *next = e->next;
            size_t idx = bucket_of(e->hash, new_n);
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    t->buckets = nb;
    t->nbuckets = new_n;
}

const char *sn_intern(SnInternTable *t, const char *s, size_t len) {
    uint64_t h = fnv1a(s, len);
    size_t idx = bucket_of(h, t->nbuckets);
    for (SnInternEntry *e = t->buckets[idx]; e; e = e->next) {
        if (e->hash == h && e->len == len && memcmp(e->text, s, len) == 0) {
            return e->text;
        }
    }

    if ((t->count + 1u) * SN_INTERN_MAX_LOAD_DEN >
        t->nbuckets * SN_INTERN_MAX_LOAD_NUM) {
        rehash(t);
        idx = bucket_of(h, t->nbuckets);
    }

    SnInternEntry *e =
        (SnInternEntry *)sn_arena_alloc(t->arena, sizeof(SnInternEntry) + len + 1u);
    e->hash = h;
    e->len = len;
    memcpy(e->text, s, len);
    e->text[len] = '\0';
    e->next = t->buckets[idx];
    t->buckets[idx] = e;
    t->count++;
    return e->text;
}

const char *sn_intern_cstr(SnInternTable *t, const char *s) {
    return sn_intern(t, s, strlen(s));
}
