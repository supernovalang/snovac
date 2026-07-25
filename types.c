#include "types.h"

#include <string.h>

#define SN_TYPES_INITIAL_BUCKETS 64u
#define SN_TYPES_MAX_LOAD_NUM 3u
#define SN_TYPES_MAX_LOAD_DEN 4u

struct SnTypeInternEntry {
    SnTypeInternEntry *next;
    uint64_t hash;
    SnTypeRep *ty;
};

/* Same mixing step as Boost's hash_combine, applied to each structural
 * field in turn — good enough distribution for the handful of fields a
 * SnTypeRep has. */
static uint64_t mix_ptr(uint64_t h, const void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    return h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
}

static uint64_t mix_u32(uint64_t h, uint32_t v) {
    return h ^ ((uint64_t)v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
}

static uint64_t hash_shape(SnTypeTag tag, const SnSymbol *decl,
                            SnTypeRep *const *args, uint32_t nargs,
                            const SnTypeRep *ret) {
    uint64_t h = 1469598103934665603ULL;
    h = mix_u32(h, (uint32_t)tag);
    h = mix_ptr(h, decl);
    h = mix_u32(h, nargs);
    for (uint32_t i = 0; i < nargs; i++) {
        h = mix_ptr(h, args[i]);
    }
    h = mix_ptr(h, ret);
    return h;
}

static int shape_equals(const SnTypeRep *ty, SnTypeTag tag,
                         const SnSymbol *decl, SnTypeRep *const *args,
                         uint32_t nargs, const SnTypeRep *ret) {
    if (ty->tag != tag || ty->decl != decl || ty->nargs != nargs ||
        ty->ret != ret) {
        return 0;
    }
    for (uint32_t i = 0; i < nargs; i++) {
        if (ty->args[i] != args[i]) {
            return 0;
        }
    }
    return 1;
}

static size_t bucket_of(uint64_t hash, size_t nbuckets) {
    return (size_t)(hash & (uint64_t)(nbuckets - 1u));
}

static void rehash(SnTypeTable *t) {
    size_t new_n = t->nbuckets * 2u;
    SnTypeInternEntry **nb = (SnTypeInternEntry **)sn_arena_calloc(
        t->arena, new_n * sizeof(SnTypeInternEntry *));
    for (size_t i = 0; i < t->nbuckets; i++) {
        SnTypeInternEntry *e = t->buckets[i];
        while (e) {
            SnTypeInternEntry *next = e->next;
            size_t idx = bucket_of(e->hash, new_n);
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    t->buckets = nb;
    t->nbuckets = new_n;
}

static SnTypeRep *intern_shape(SnTypeTable *t, SnTypeTag tag, SnSymbol *decl,
                                SnTypeRep **args, uint32_t nargs,
                                SnTypeRep *ret) {
    uint64_t h = hash_shape(tag, decl, args, nargs, ret);
    size_t idx = bucket_of(h, t->nbuckets);
    for (SnTypeInternEntry *e = t->buckets[idx]; e; e = e->next) {
        if (e->hash == h && shape_equals(e->ty, tag, decl, args, nargs, ret)) {
            return e->ty;
        }
    }

    if ((t->count + 1u) * SN_TYPES_MAX_LOAD_DEN >
        t->nbuckets * SN_TYPES_MAX_LOAD_NUM) {
        rehash(t);
        idx = bucket_of(h, t->nbuckets);
    }

    SnTypeRep *ty = (SnTypeRep *)sn_arena_calloc(t->arena, sizeof(SnTypeRep));
    ty->tag = tag;
    ty->decl = decl;
    ty->nargs = nargs;
    ty->ret = ret;
    if (nargs) {
        ty->args =
            (SnTypeRep **)sn_arena_alloc(t->arena, nargs * sizeof(SnTypeRep *));
        memcpy(ty->args, args, nargs * sizeof(SnTypeRep *));
    } else {
        ty->args = NULL;
    }

    SnTypeInternEntry *e =
        (SnTypeInternEntry *)sn_arena_alloc(t->arena, sizeof(SnTypeInternEntry));
    e->hash = h;
    e->ty = ty;
    e->next = t->buckets[idx];
    t->buckets[idx] = e;
    t->count++;
    return ty;
}

void sn_types_init(SnTypeTable *t, SnArena *a) {
    t->arena = a;
    t->nbuckets = SN_TYPES_INITIAL_BUCKETS;
    t->buckets = (SnTypeInternEntry **)sn_arena_calloc(
        a, t->nbuckets * sizeof(SnTypeInternEntry *));
    t->count = 0;

    /* Primitives are hash-consed the same way as everything else (nargs=0,
     * decl=NULL, ret=NULL differ only by tag) and then cached directly, since
     * every caller wants the one instance of each and there is no reason to
     * make them look the constructors up repeatedly. */
    t->error_ty = intern_shape(t, SN_T_ERROR, NULL, NULL, 0, NULL);
    t->unit_ty = intern_shape(t, SN_T_UNIT, NULL, NULL, 0, NULL);
    t->bool_ty = intern_shape(t, SN_T_BOOL, NULL, NULL, 0, NULL);
    t->int_ty = intern_shape(t, SN_T_INT, NULL, NULL, 0, NULL);
    t->long_ty = intern_shape(t, SN_T_LONG, NULL, NULL, 0, NULL);
    t->double_ty = intern_shape(t, SN_T_DOUBLE, NULL, NULL, 0, NULL);
    t->decimal_ty = intern_shape(t, SN_T_DECIMAL, NULL, NULL, 0, NULL);
    t->string_ty = intern_shape(t, SN_T_STRING, NULL, NULL, 0, NULL);
    t->char_ty = intern_shape(t, SN_T_CHAR, NULL, NULL, 0, NULL);
    t->float_ty = intern_shape(t, SN_T_FLOAT, NULL, NULL, 0, NULL);
    t->byte_ty = intern_shape(t, SN_T_BYTE, NULL, NULL, 0, NULL);
    t->any_ty = intern_shape(t, SN_T_ANY, NULL, NULL, 0, NULL);
}

SnTypeRep *sn_type_error(SnTypeTable *t) { return t->error_ty; }
SnTypeRep *sn_type_unit(SnTypeTable *t) { return t->unit_ty; }
SnTypeRep *sn_type_bool(SnTypeTable *t) { return t->bool_ty; }
SnTypeRep *sn_type_int(SnTypeTable *t) { return t->int_ty; }
SnTypeRep *sn_type_long(SnTypeTable *t) { return t->long_ty; }
SnTypeRep *sn_type_double(SnTypeTable *t) { return t->double_ty; }
SnTypeRep *sn_type_decimal(SnTypeTable *t) { return t->decimal_ty; }
SnTypeRep *sn_type_string(SnTypeTable *t) { return t->string_ty; }
SnTypeRep *sn_type_char(SnTypeTable *t) { return t->char_ty; }
SnTypeRep *sn_type_float(SnTypeTable *t) { return t->float_ty; }
SnTypeRep *sn_type_byte(SnTypeTable *t) { return t->byte_ty; }
SnTypeRep *sn_type_any(SnTypeTable *t) { return t->any_ty; }

int sn_type_is_any(const SnTypeRep *t) { return t && t->tag == SN_T_ANY; }

SnTypeRep *sn_type_named(SnTypeTable *t, SnSymbol *decl, SnTypeRep **args,
                          uint32_t nargs) {
    return intern_shape(t, SN_T_NAMED, decl, args, nargs, NULL);
}

SnTypeRep *sn_type_typevar(SnTypeTable *t, SnSymbol *decl) {
    return intern_shape(t, SN_T_TYPEVAR, decl, NULL, 0, NULL);
}

SnTypeRep *sn_type_func(SnTypeTable *t, SnTypeRep **params, uint32_t nparams,
                         SnTypeRep *ret) {
    return intern_shape(t, SN_T_FUNC, NULL, params, nparams, ret);
}

SnTypeRep *sn_type_array(SnTypeTable *t, SnTypeRep *elem) {
    SnTypeRep *args[1];
    args[0] = elem;
    return intern_shape(t, SN_T_ARRAY, NULL, args, 1u, NULL);
}

int sn_type_equals(const SnTypeRep *a, const SnTypeRep *b) { return a == b; }
