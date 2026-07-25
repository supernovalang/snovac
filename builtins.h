/* builtins.h — intrinsic types and their members.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/llm.md, P2.6 "Por que 8/26 e
 * não mais": the two largest remaining causes of fixture errors were `Array`
 * (a generic type no .snova file declares) and the absence of any member table
 * for primitives (`n.toString()`, `arr.len()`). Both live here.
 *
 * The authoritative list is crates/snovalang/src/native/selfcheck/
 * static_facts.rs (`seed_builtins`) — the Stage 0 frontend that `snova check`
 * actually runs today. Its table records member NAMES only, with no
 * signatures, so this module records:
 *
 *   - the member's RESULT type, modeled as precisely as the name allows
 *     (`len` -> int, `join` -> string, `get` -> the array's element type) and
 *     as `any` where it genuinely isn't determined by the name alone
 *     (`reduce`, `map`'s callback result). `any` never participates in a
 *     mismatch (sn_type_is_any), so an imprecise entry stays silent rather
 *     than producing a wrong diagnostic;
 *
 *   - NO parameter list. Nothing upstream declares one, and inventing
 *     signatures for `map`/`substring`/`split` would fabricate arity rules the
 *     language has never stated. A builtin method is therefore reported as
 *     SN_T_FUNC with zero parameters, and check.c does not check the argument
 *     list of a call whose callee resolved through this module. That is a
 *     documented gap, not an oversight: argument checking for builtin members
 *     needs real signatures first.
 *
 * `Partial<T>` is modeled as T itself — see sn_builtin_generic().
 */
#ifndef SNOVAC_BUILTINS_H
#define SNOVAC_BUILTINS_H

#include "intern.h"
#include "types.h"

/* Generic type constructors that no .snova file declares.
 *
 *   Array<T>   -> SN_T_ARRAY with element T (bare `Array` -> Array<any>)
 *   Partial<T> -> T
 *
 * `Partial<T>` is Stage 0's compiler-synthesized "every field seen as
 * Option<FieldType>" view (tests/compile-pass/partial_type.snova). Modeling it
 * as T is deliberate and is what both fixtures actually require: the accepted
 * fixture writes `user.age.toString()` — the field used at its own type, not
 * wrapped — and the rejected one
 * (tests/compile-fail/partial_type_unknown_field.snova) only needs an unknown
 * field to stay unknown, which member lookup on T already gives. The
 * Option-wrapping itself is not modeled.
 *
 * Returns NULL when `iname` is not an intrinsic generic. `args` are the
 * caller's already-resolved generic arguments and are not retained. */
SnTypeRep *sn_builtin_generic(SnTypeTable *t, SnInternTable *it, const char *iname,
                              SnTypeRep **args, uint32_t nargs);

/* True for exactly the names sn_builtin_generic answers to. Callers use it to
 * decide whether to resolve a generic argument list at all, so that a
 * non-intrinsic name keeps reporting one diagnostic for itself instead of also
 * reporting its unresolved arguments. */
int sn_builtin_is_generic_name(SnInternTable *it, const char *iname);

/* Type of `recv.<iname>` for an intrinsic receiver — an array, a string, or a
 * numeric/bool primitive. Methods come back as SN_T_FUNC with zero parameters
 * and the modeled result in `ret` (see the header note on why the parameter
 * list is empty rather than absent). Returns NULL when `recv` has no such
 * intrinsic member, which is the caller's cue to report SNOVA0028 as before. */
SnTypeRep *sn_builtin_member(SnTypeTable *t, SnInternTable *it, const SnTypeRep *recv,
                             const char *iname);

/* Element type produced by `recv[i]`, or NULL when `recv` is not subscriptable.
 *
 * The set of subscriptable receivers is
 * static_facts.rs::invalid_subscript_receiver_type, read positively: string,
 * Bytes, JsonValue, JsonArray, Array<T>, Map<K, V>, List<T>. Where the
 * receiver's generic arguments say what comes out, they are used
 * (`Map<string, int>["a"]` is an `int`); otherwise the result is `any` rather
 * than a guess. */
SnTypeRep *sn_builtin_index_result(SnTypeTable *t, SnInternTable *it,
                                   const SnTypeRep *recv);

/* Type of `<Intrinsic>.<iname>` where the receiver is the intrinsic TYPE, not
 * a value: `Array<Post>.new()`. `recv` is the type as built by
 * sn_builtin_generic. Same SN_T_FUNC convention as sn_builtin_member. */
SnTypeRep *sn_builtin_static_member(SnTypeTable *t, SnInternTable *it,
                                    const SnTypeRep *recv, const char *iname);

#endif /* SNOVAC_BUILTINS_H */
