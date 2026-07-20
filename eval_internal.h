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
    V_UNIT, V_INT, V_DOUBLE, V_BOOL, V_STRING, V_OBJECT
} ValKind;

typedef struct Object Object;

typedef struct {
    ValKind kind;
    union {
        long long i;
        double d;
        int b;
        const char *s;
        Object *o;
    } as;
} Value;

struct Object {
    const SnDecl *cls;
    SnList names;  /* const char* */
    SnList slots;  /* Value* */
};

typedef enum { FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE } Flow;

typedef struct Env {
    struct Env *parent;
    SnList names; /* const char* */
    SnList slots; /* Value* — boxed so assignment is visible to inner scopes */
} Env;

typedef struct {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
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

/* ── eval_expr.c ──────────────────────────────────────────────────────────── */

long long as_int(Interp *in, Value v, SnSpan span);
int truthy(Interp *in, Value v, SnSpan span);
Value eval_expr(Interp *in, Env *env, const SnExpr *e);

/* ── eval_stmt.c ──────────────────────────────────────────────────────────── */

Flow exec_stmt(Interp *in, Env *env, const SnStmt *s);

/* ── eval_string.c ────────────────────────────────────────────────────────── */

/* Decodes a string literal token: strips the quotes, resolves escapes, and
 * evaluates `${...}` interpolation by parsing the inner expression source.
 * `$$` is a literal `$` and never starts interpolation. */
const char *decode_string(Interp *in, Env *env, const SnExpr *e);

#endif /* SNOVAC_EVAL_INTERNAL_H */
