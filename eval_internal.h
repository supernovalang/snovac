/* eval_internal.h — shared state for the tree-walking interpreter.
 *
 * Split by concern to keep every translation unit small:
 *   eval.c         entry point, environments, object model, calls
 *   eval_expr.c    expression evaluation
 *   eval_stmt.c    statement execution
 *   eval_string.c  literal decoding and `${...}` interpolation
 *
 * This interpreter is the P1 smoke path — `snovac run` — not the P3 VM. It
 * exists so parser output can be executed end to end while the bytecode
 * backend is still ahead of us.
 */
#ifndef SNOVAC_EVAL_INTERNAL_H
#define SNOVAC_EVAL_INTERNAL_H

#include "eval.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runtime band: 0300-0399 */
#define SNOVA_NO_MAIN            300
#define SNOVA_UNDEFINED_NAME     301
#define SNOVA_NOT_CALLABLE       302
#define SNOVA_UNKNOWN_INTRINSIC  303
#define SNOVA_TYPE_ERROR         304
#define SNOVA_UNSUPPORTED        305

typedef enum {
    V_UNIT, V_INT, V_DOUBLE, V_BOOL, V_STRING, V_OBJECT, V_VARIANT, V_LAMBDA, V_ARRAY
} ValKind;

typedef struct Object Object;
typedef struct VariantVal VariantVal;
typedef struct LambdaVal LambdaVal;
typedef struct ArrayVal ArrayVal;

struct ArrayVal {
    SnList items; /* Value* */
};

typedef struct {
    ValKind kind;
    union {
        long long i;
        double d;
        int b;
        const char *s;
        Object *o;
        VariantVal *vt;
        LambdaVal *lam;
        ArrayVal *arr;
    } as;
} Value;

struct Object {
    const SnDecl *cls;
    SnList names;  /* const char* */
    SnList slots;  /* Value* */
};

/* A constructed enum variant — `Some(x)`, `Ok(v)`, `Err(e)`, `None`, or a
 * variant of a user-declared enum. `name` is the bare variant name; `payload`
 * holds the constructor arguments in order (Value*). */
struct VariantVal {
    const char *name;
    SnList payload; /* Value* */
};

typedef enum { FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE } Flow;

typedef struct Env {
    struct Env *parent;
    SnList names; /* const char* */
    SnList slots; /* Value* — boxed so assignment is visible to inner scopes */
} Env;

/* A first-class `(x) -> ...` value: the lambda expression plus the environment
 * it closed over. */
struct LambdaVal {
    const SnExpr *expr; /* SN_EXPR_LAMBDA node (params, value/body) */
    Env *env;           /* captured lexical environment */
};

typedef struct {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
    Env *globals;
    Value ret;
    Flow flow;
    int failed;
} Interp;

/* ── value constructors ───────────────────────────────────────────────────── */

static inline Value v_unit(void)         { Value v; v.kind = V_UNIT; v.as.i = 0; return v; }
static inline Value v_int(long long i)   { Value v; v.kind = V_INT; v.as.i = i; return v; }
static inline Value v_double(double d)   { Value v; v.kind = V_DOUBLE; v.as.d = d; return v; }
static inline Value v_bool(int b)        { Value v; v.kind = V_BOOL; v.as.b = b != 0; return v; }
static inline Value v_str(const char *s) { Value v; v.kind = V_STRING; v.as.s = s; return v; }

/* ── eval.c ───────────────────────────────────────────────────────────────── */

void rt_error(Interp *in, int code, SnSpan span, const char *fmt, ...);

Env *env_new(Interp *in, Env *parent);
Value *env_lookup(Env *e, const char *name);
Value *env_define(Interp *in, Env *e, const char *name, Value v);

char *arena_sprintf(Interp *in, const char *fmt, ...);
char *str_concat(Interp *in, const char *a, const char *b);
const char *to_string(Interp *in, Value v, SnSpan span);

const SnDecl *find_member(const SnDecl *cls, const char *name);
const SnDecl *find_member_inherited(const Interp *in, const SnDecl *cls, const char *name);
const SnDecl *find_top(const Interp *in, const char *name, SnDeclKind k);
const SnDecl *find_type(const Interp *in, const char *name);

Value default_for(const SnType *t);
Object *instantiate(Interp *in, const SnDecl *cls, SnList *args, Env *env,
                    SnSpan span);
Value *object_field(Object *o, const char *name);

Value call_function(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                    Object *self, SnSpan span);
Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                  Env *caller, SnSpan span);
int sn_native_try_dispatch(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                           Object *self, SnSpan span, Value *out);

/* ── variants, lambdas and pattern matching ───────────────────────────────── */

Value v_variant(Interp *in, const char *name, SnList payload);
Value call_lambda(Interp *in, const LambdaVal *lam, SnList *args, Env *caller,
                  SnSpan span);
int value_equals(Value a, Value b);
/* Attempts to match `subject` against `pat`; bindings are defined in `env`.
 * Returns 1 on match. Bindings from a failed partial match may remain in
 * `env` — callers pass a fresh child env per arm. */
int pattern_match_bind(Interp *in, Env *env, const SnPattern *pat,
                       Value subject);
/* Shared match driver: finds the first matching arm (patterns + guard) and
 * returns it, with `*arm_env` set to the env holding its bindings. NULL when
 * no arm matches. */
const SnMatchArm *match_select_arm(Interp *in, Env *env, const SnList *arms,
                                   Value subject, Env **arm_env);

/* ── eval_expr.c ──────────────────────────────────────────────────────────── */

long long as_int(Interp *in, Value v, SnSpan span);
int truthy(Interp *in, Value v, SnSpan span);
Value eval_expr(Interp *in, Env *env, const SnExpr *e);
/* `Some`/`Ok`/`Err`/`None`, or a variant of a declared enum. Shared with
 * pattern_match_bind(): a bare pattern name is only a wildcard bind when it
 * is NOT one of these — see the parser's note in parse_pattern(). */
int is_variant_constructor(Interp *in, const char *name);

/* ── eval_stmt.c ──────────────────────────────────────────────────────────── */

Flow exec_stmt(Interp *in, Env *env, const SnStmt *s);

/* ── eval_string.c ────────────────────────────────────────────────────────── */

/* Decodes a string literal token: strips the quotes, resolves escapes, and
 * evaluates `${...}` interpolation by parsing the inner expression source.
 * `$$` is a literal `$` and never starts interpolation. */
const char *decode_string(Interp *in, Env *env, const SnExpr *e);

#endif /* SNOVAC_EVAL_INTERNAL_H */
