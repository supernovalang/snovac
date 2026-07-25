/* types.h — type representation, hash-consed.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §5 / §8 step 5. The
 * shape below (tag, decl, args/nargs, ret) is exactly what plan.md
 * specifies; two fields are reused for tags the struct doesn't name a field
 * for, documented at each constructor:
 *   - SN_T_ARRAY has no dedicated "element type" field, so it reuses
 *     args[0]/nargs=1 (decl and ret stay NULL). sn_type_array() hides this.
 *   - SN_T_FUNC has no dedicated "parameter list" field, so it reuses
 *     args/nargs for parameter types and `ret` for the return type, exactly
 *     as plan.md's own comment says ("ret: for SN_T_FUNC").
 *
 * Every constructor here is hash-consed: two calls with structurally equal
 * arguments always return the same SnTypeRep pointer, so `==` is a valid
 * type-equality check anywhere in the compiler — plan.md's example is
 * `List<int> == List<int>` by pointer. This works recursively without deep
 * comparison: nargs/args/ret only ever hold pointers that are themselves
 * already hash-consed, so comparing immediate fields is enough.
 *
 * SN_T_ERROR is a single canonical instance (sn_type_error()). Silent
 * propagation ("an operation on SN_T_ERROR produces SN_T_ERROR without a
 * second diagnostic", plan.md §5.1) is check.c's (P2.5) responsibility, not
 * this module's — types.c only guarantees the singleton exists to compare
 * against.
 */
#ifndef SNOVAC_TYPES_H
#define SNOVAC_TYPES_H

#include <stdint.h>

#include "arena.h"
#include "symbol.h"

typedef enum {
    SN_T_ERROR = 0, /* propagates silently; never a real diagnostic target */
    SN_T_UNIT, SN_T_BOOL, SN_T_INT, SN_T_LONG,
    SN_T_DOUBLE, SN_T_DECIMAL, SN_T_STRING, SN_T_CHAR,
    SN_T_FLOAT, SN_T_BYTE,
    SN_T_ANY,     /* the corpus's escape hatch — `func printline(value: any)`
                    * in builtin/Console.snova. A real, resolvable type, but
                    * one that never takes part in a mismatch (sn_type_is_any) */
    SN_T_NAMED,   /* class/struct/enum/interface, with generic args */
    SN_T_TYPEVAR, /* type parameter T within a generic scope */
    SN_T_FUNC,    /* (A, B) -> C — lambdas and function references */
    SN_T_ARRAY    /* element type in args[0] — see file header */
} SnTypeTag;

typedef struct SnTypeRep {
    SnTypeTag tag;
    SnSymbol *decl;          /* SN_T_NAMED / SN_T_TYPEVAR */
    struct SnTypeRep **args; /* SN_T_NAMED: generic args; SN_T_FUNC: params;
                               * SN_T_ARRAY: args[0] is the element type */
    uint32_t nargs;
    struct SnTypeRep *ret;   /* SN_T_FUNC only */
} SnTypeRep;

typedef struct SnTypeInternEntry SnTypeInternEntry;

typedef struct {
    SnArena *arena;
    SnTypeInternEntry **buckets;
    size_t nbuckets;
    size_t count;

    /* Primitive singletons, built once by sn_types_init(). */
    SnTypeRep *error_ty, *unit_ty, *bool_ty, *int_ty, *long_ty, *double_ty,
        *decimal_ty, *string_ty, *char_ty, *float_ty, *byte_ty, *any_ty;
} SnTypeTable;

void sn_types_init(SnTypeTable *t, SnArena *a);

SnTypeRep *sn_type_error(SnTypeTable *t);
SnTypeRep *sn_type_unit(SnTypeTable *t);
SnTypeRep *sn_type_bool(SnTypeTable *t);
SnTypeRep *sn_type_int(SnTypeTable *t);
SnTypeRep *sn_type_long(SnTypeTable *t);
SnTypeRep *sn_type_double(SnTypeTable *t);
SnTypeRep *sn_type_decimal(SnTypeTable *t);
SnTypeRep *sn_type_string(SnTypeTable *t);
SnTypeRep *sn_type_char(SnTypeTable *t);
SnTypeRep *sn_type_float(SnTypeTable *t);
SnTypeRep *sn_type_byte(SnTypeTable *t);
SnTypeRep *sn_type_any(SnTypeTable *t);

/* True for `any` — the checker must not report a mismatch against it in
 * either direction. Kept next to the type model rather than inside check.c so
 * every phase agrees on what `any` means. */
int sn_type_is_any(const SnTypeRep *t);

/* `args` is copied (the caller's array does not need to outlive the call). */
SnTypeRep *sn_type_named(SnTypeTable *t, SnSymbol *decl, SnTypeRep **args,
                          uint32_t nargs);
SnTypeRep *sn_type_typevar(SnTypeTable *t, SnSymbol *decl);
SnTypeRep *sn_type_func(SnTypeTable *t, SnTypeRep **params, uint32_t nparams,
                         SnTypeRep *ret);
SnTypeRep *sn_type_array(SnTypeTable *t, SnTypeRep *elem);

/* Pointer equality — provided so call sites can read `sn_type_equals(a, b)`
 * instead of a bare `a == b` next to non-hash-consed pointer comparisons,
 * not because it does anything `==` doesn't. */
int sn_type_equals(const SnTypeRep *a, const SnTypeRep *b);

#endif /* SNOVAC_TYPES_H */
