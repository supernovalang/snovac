/* check.h — type checking over expressions and statement bodies.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §4.4/§5, §8 steps
 * 7-8. Runs after resolve.c: needs sn_resolve_ident/sn_resolve_type_name for
 * every name it encounters, and needs sn_resolver_collect() already done so
 * package/type member scopes exist.
 *
 * ── Scope actually covered by this pass (2026-07-25) ────────────────────────
 *
 * Solid, tested: literals, identifiers (incl. through single-inheritance),
 * member access (incl. inherited, and the intrinsic members builtins.c owns —
 * `arr.len()`, `n.toString()`), calls (arity + argument type checking, incl.
 * enum variant construction like `Some(x)`/`Ok(v)`, construction of a
 * class/struct, and calling a value of function type), binary/unary
 * operators under the ratified no-implicit-promotion rule, assignment
 * (incl. immutability — SNOVA_IMMUTABLE_REASSIGN — and its constructor
 * exception), index (arrays plus the other subscriptable receivers in
 * builtins.c), cast (`as`, trusts the annotation), `is` (validates the type
 * exists, returns bool), array literals (incl. the SNOVA100 empty-literal
 * rule and its annotated-binding exception), `let`/`var` inference,
 * `if`/`while` condition-is-bool, `return`-vs-signature matching, `this`,
 * subtype assignability, and the intrinsic generic types `Array<T>` /
 * `Partial<T>`.
 *
 * Two rules that read like exceptions but are not looseness, each carrying
 * its own justification at the implementation site:
 *
 *   - an UNSUFFIXED numeric literal adopts the expected type
 *     (adopt_literal_type). The ratified no-implicit-promotion rule is about
 *     converting an already-typed value and is unaffected — `int + long` is
 *     still an error, and a SUFFIXED literal (`42L`, `1.5d`) is committed.
 *   - a call whose callee resolved through builtins.c does NOT have its
 *     argument list checked, because that module models no parameters. See
 *     builtins.h for why inventing them was rejected.
 *
 * Documented, NOT covered (returns SN_T_ERROR for the construct's own type,
 * but still recurses into children so nested identifiers/calls are checked
 * and contribute to coverage — these are not silently wrong, they're
 * honestly untyped):
 *
 *   - Lambdas without a fully-annotated parameter list. plan.md §5.3 asks
 *     for bidirectional inference from the expected type at the use site;
 *     not implemented. Fully-annotated lambdas ARE checked.
 *   - `match`/`if`-as-expression/struct-literal as a VALUE — their own
 *     result type isn't computed (arm/branch bodies are still checked).
 *     Match-arm pattern bindings are registered but with SN_T_ERROR — real
 *     pattern-to-payload typing isn't implemented.
 *   - `for (let x in xs)` — no Iterable/element-type protocol is modeled;
 *     `x` is bound with SN_T_ERROR.
 *   - `await`: passes the operand's type through unchanged unless it's
 *     literally named `Task` with a generic argument, in which case it
 *     unwraps to that argument. No real async/Task modeling.
 *   - `?` propagation: plan.md's ratified-ambiguity list (§7 item 6) was
 *     left OPEN by resolve.c — corpus evidence is essentially absent. This
 *     is a best-effort, explicitly PROVISIONAL implementation (unwraps
 *     Option<T>/Result<T,E> by name), not a confirmed design.
 *   - String interpolation contents (`"${expr}"`) are not statically typed
 *     — the AST doesn't expose the interpolated sub-expressions as children
 *     (they're re-parsed from raw text at eval time, in eval_string.c); a
 *     string literal's own type is always `string` regardless.
 *   - Generics: arguments are threaded through named types structurally
 *     (List<int> vs List<string> are genuinely different, hash-consed,
 *     checked types), but nothing here checks a generic parameter's bound,
 *     substitutes T inside a method's declared types when called on a
 *     concrete instantiation, or infers T from usage. A method declared to
 *     return `T` on `List<T>` type-checks as returning the type variable T
 *     itself, not the instance's concrete element type.
 */
#ifndef SNOVAC_CHECK_H
#define SNOVAC_CHECK_H

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"
#include "resolve.h"
#include "symbol.h"
#include "types.h"

/* New in this module. 108/109/120 (package.c/resolve.c) and the ranges
 * documented there are already spoken for; 131-140 confirmed free before
 * picking these. */
#define SNOVA_ARITY_MISMATCH 131
#define SNOVA_ARG_TYPE_MISMATCH 132
#define SNOVA_BINARY_TYPE_MISMATCH 133
#define SNOVA_UNARY_TYPE_MISMATCH 134
#define SNOVA_CONDITION_NOT_BOOL 135
#define SNOVA_RETURN_TYPE_MISMATCH 136
#define SNOVA_NOT_CALLABLE 137
#define SNOVA_LET_TYPE_MISMATCH 138
#define SNOVA_BREAK_OUTSIDE_LOOP 139
#define SNOVA_CONTINUE_OUTSIDE_LOOP 140
#define SNOVA_ABSTRACT_INSTANTIATION 141
#define SNOVA_MISSING_RETURN 142
#define SNOVA_PRIVATE_ACCESS 143
#define SNOVA_GENERIC_ARITY_MISMATCH 144

/* Canonical codes, not new: already documented in docs/snovalang-diagnostics.md
 * and implemented in crates/snovalang/src/native/selfcheck/pulsar.rs
 * (Stage 0's text-scanner mirror of the same rules) — snovac had never
 * implemented its own AST-based version of either check. */
#define SNOVA_PULSAR_INVALID_RETURN 122
#define SNOVA_PULSAR_DIRECT_CALL 124
#define SNOVA_PULSAR_IN_ASYNC 125
#define SNOVA_AWAIT_OUTSIDE_ASYNC 13

#define SNOVA_OPTIONAL_TYPE_REQUIRED    200
#define SNOVA_REDUNDANT_NULL_COALESCING 201
#define SNOVA_MISMATCHED_FALLBACK       202
#define SNOVA_OPTIONAL_CHAINING_NON_OPTIONAL 203
#define SNOVA_NESTED_OPTIONAL           204
#define SNOVA_NON_EXHAUSTIVE_MATCH      205
#define SNOVA_ANY_IN_PUBLIC_LIBRARY     210
#define SNOVA_ANY_IMPLICITLY_INFERRED   211
#define SNOVA_INFINITE_LOOP             215
#define SNOVA_EMPTY_LOOP                216
#define SNOVA_PROHIBITED_NATIVE         220

/* Reused as-is — already fixture-backed with exactly this meaning. */
#define SNOVA_UNKNOWN_MEMBER 28
#define SNOVA_IMMUTABLE_REASSIGN 47
#define SNOVA_UNTYPED_VARIABLE 100

typedef struct {
    SnArena *arena;
    SnInternTable *intern;
    SnDiagSink *diag;
    SnResolver *resolver;
    SnTypeTable *types;

    /* Set by the driver before checking a given file/decl; sn_check_expr and
     * friends read these but never change them (sn_check_decl_body only
     * touches current_return_type, saving/restoring it isn't needed since
     * bodies never nest). */
    const char *current_package;
    const SnList *current_imports; /* interned package names, or NULL */
    const SnDecl *enclosing_type;  /* NULL at top level */
    SnTypeRep *current_return_type;

    /* Type parameters in scope for the body being checked — the declaration's
     * own `generics` plus, for a method, its owning type's. `T` in
     * `func try<T, E>(result: Result<T, E>)` is not a name any package
     * declares, so without this it resolved to nothing and every generic
     * signature in the corpus reported SNOVA0027. NULL outside a body. */
    SnScope *type_params;

    /* Set while checking a `constructor(...)` member's body. A `let` field is
     * immutable everywhere else, but the constructor is where it receives its
     * value (`this.description = description`) — see is_mutable_symbol. */
    int in_constructor;

    /* Set for the whole body of an `async func`/`async method` — SNOVA013
     * needs it to reject `await` anywhere else. */
    int in_async_body;

    /* Set only around checking the expr operand of a `pulsar work()`
     * statement (SN_STMT_PULSAR) — the one place a pulsar function may be
     * called directly. Consumed (read then cleared) the moment the CALL
     * expression's callee is resolved, so it never leaks into that call's
     * own arguments or into unrelated nested calls. */
    int in_pulsar_launch;

    /* Nesting depth of `while`/`for` bodies being checked — 0 outside any
     * loop. `break`/`continue` are only legal at depth > 0. */
    int loop_depth;
} SnChecker;

void sn_checker_init(SnChecker *c, SnArena *a, SnInternTable *it, SnDiagSink *diag,
                      SnResolver *resolver, SnTypeTable *types);

/* Resolves an AST type reference (SnType, as written in source) to a
 * SnTypeRep. Exposed because resolve.c/check.c's callers (a future
 * corpus-wide driver, or check.c's own tests) need it for member/param/
 * return types, not just expressions. */
SnTypeRep *sn_check_resolve_type(SnChecker *c, const SnType *t);

/* Computes and caches (on `e->resolved_type`) the type of one expression.
 * Never returns NULL — an expression that can't be typed gets
 * sn_type_error(), and per plan.md §5.1 no operation that CONSUMES an
 * already-error-typed operand emits a second diagnostic. `local` is the
 * innermost lexical scope. */
SnTypeRep *sn_check_expr(SnChecker *c, SnScope *local, SnExpr *e);

/* Checks one statement in place. LET/VAR define directly into `local` —
 * callers open a fresh child scope per block (sn_check_block does this) so
 * bindings don't leak across sibling blocks. */
void sn_check_stmt(SnChecker *c, SnScope *local, SnStmt *s);

/* Checks a BLOCK statement's contents in a fresh child scope of `parent`. */
void sn_check_block(SnChecker *c, SnScope *parent, SnStmt *block);

/* Entry point: checks one FUNC/METHOD's body, if it has one. A bodyless
 * declaration (bare signature, or @native) is accepted without checking —
 * existence of the native implementation is P5's problem, per plan.md's
 * explicit non-objective. `current_package`/`current_imports`/
 * `enclosing_type` must already be set on `c`. */
void sn_check_decl_body(SnChecker *c, const SnDecl *decl);

#endif /* SNOVAC_CHECK_H */
