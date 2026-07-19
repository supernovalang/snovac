#include "eval.h"

#include "lex.h"
#include "parse.h"

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

/* ── values ───────────────────────────────────────────────────────────────── */

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

static Value v_unit(void)          { Value v; v.kind = V_UNIT; v.as.i = 0; return v; }
static Value v_int(long long i)    { Value v; v.kind = V_INT; v.as.i = i; return v; }
static Value v_double(double d)    { Value v; v.kind = V_DOUBLE; v.as.d = d; return v; }
static Value v_bool(int b)         { Value v; v.kind = V_BOOL; v.as.b = b != 0; return v; }
static Value v_str(const char *s)  { Value v; v.kind = V_STRING; v.as.s = s; return v; }

static void rt_error(Interp *in, int code, SnSpan span, const char *fmt, ...) {
    if (in->failed) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sn_diag_emit(in->diag, SN_DIAG_ERROR, code, span, "%s", buf);
    in->failed = 1;
    in->flow = FLOW_RETURN;
}

/* ── environment ──────────────────────────────────────────────────────────── */

static Env *env_new(Interp *in, Env *parent) {
    Env *e = (Env *)sn_arena_calloc(in->arena, sizeof(Env));
    e->parent = parent;
    return e;
}

static Value *env_lookup(Env *e, const char *name) {
    for (; e; e = e->parent) {
        for (size_t i = 0; i < e->names.len; i++) {
            if (strcmp((const char *)e->names.items[i], name) == 0) {
                return (Value *)e->slots.items[i];
            }
        }
    }
    return NULL;
}

static Value *env_define(Interp *in, Env *e, const char *name, Value v) {
    Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
    *slot = v;
    sn_list_push(in->arena, &e->names, (void *)name);
    sn_list_push(in->arena, &e->slots, slot);
    return slot;
}

/* ── strings ──────────────────────────────────────────────────────────────── */

static char *arena_sprintf(Interp *in, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    return sn_arena_strndup(in->arena, buf, (size_t)n < sizeof(buf) ? (size_t)n
                                                                   : sizeof(buf) - 1);
}

static char *str_concat(Interp *in, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *out = (char *)sn_arena_alloc(in->arena, la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

static Value eval_expr(Interp *in, Env *env, const SnExpr *e);
static Flow exec_stmt(Interp *in, Env *env, const SnStmt *s);
static Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                         Env *caller, SnSpan span);
static const SnDecl *find_member(const SnDecl *cls, const char *name);

static const char *to_string(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_UNIT:   return "unit";
    case V_INT:    return arena_sprintf(in, "%lld", v.as.i);
    case V_DOUBLE: return arena_sprintf(in, "%g", v.as.d);
    case V_BOOL:   return v.as.b ? "true" : "false";
    case V_STRING: return v.as.s;
    case V_OBJECT: {
        /* Printing an object uses its `asString()`, matching the convention the
         * corpus relies on (tests/run-pass/counter.snova). */
        const SnDecl *m = find_member(v.as.o->cls, "asString");
        if (m && m->body) {
            SnList none = {0};
            Value s = call_method(in, v.as.o, m, &none, NULL, span);
            if (s.kind == V_STRING) {
                return s.as.s;
            }
        }
        return arena_sprintf(in, "<%s>", v.as.o->cls->name ? v.as.o->cls->name : "object");
    }
    }
    return "?";
}

/* Decodes a string literal token: strips the quotes, resolves escapes, and
 * evaluates `${...}` interpolation by parsing the inner expression source.
 * `$$` is a literal `$` and never starts interpolation. */
static const char *decode_string(Interp *in, Env *env, const SnExpr *e);

/* ── declarations lookup ──────────────────────────────────────────────────── */

static const SnDecl *find_member(const SnDecl *cls, const char *name) {
    if (!cls) {
        return NULL;
    }
    for (size_t i = 0; i < cls->members.len; i++) {
        const SnDecl *m = (const SnDecl *)cls->members.items[i];
        if (m->name && strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

static const SnDecl *find_top(const Interp *in, const char *name, SnDeclKind k) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if (d->kind == k && d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

static const SnDecl *find_type(const Interp *in, const char *name) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if ((d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT) && d->name &&
            strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

/* ── objects ──────────────────────────────────────────────────────────────── */

static Value default_for(const SnType *t) {
    if (t && t->name) {
        if (strcmp(t->name, "int") == 0 || strcmp(t->name, "long") == 0) return v_int(0);
        if (strcmp(t->name, "double") == 0 || strcmp(t->name, "decimal") == 0) return v_double(0);
        if (strcmp(t->name, "bool") == 0) return v_bool(0);
        if (strcmp(t->name, "string") == 0) return v_str("");
    }
    return v_unit();
}

static Object *instantiate(Interp *in, const SnDecl *cls, SnList *args, Env *env,
                           SnSpan span) {
    Object *o = (Object *)sn_arena_calloc(in->arena, sizeof(Object));
    o->cls = cls;

    /* Fields are declared in order; positional constructor arguments fill them
     * in that same order (`LiveWorkbook(path, frame, false)`). */
    size_t argi = 0;
    for (size_t i = 0; i < cls->members.len; i++) {
        const SnDecl *m = (const SnDecl *)cls->members.items[i];
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        Value v;
        if (m->init) {
            v = eval_expr(in, env, m->init);
        } else if (args && argi < args->len) {
            v = eval_expr(in, env, (const SnExpr *)args->items[argi]);
            argi++;
        } else {
            v = default_for(m->type);
        }
        Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
        *slot = v;
        sn_list_push(in->arena, &o->names, (void *)m->name);
        sn_list_push(in->arena, &o->slots, slot);
    }
    (void)span;
    return o;
}

static Value *object_field(Object *o, const char *name) {
    for (size_t i = 0; i < o->names.len; i++) {
        if (strcmp((const char *)o->names.items[i], name) == 0) {
            return (Value *)o->slots.items[i];
        }
    }
    return NULL;
}

/* ── calls ────────────────────────────────────────────────────────────────── */

static Value call_function(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                           Object *self, SnSpan span) {
    if (!fn->body) {
        rt_error(in, SNOVA_UNKNOWN_INTRINSIC, span,
                 "`%s` has no body and its @native intrinsic is not implemented yet",
                 fn->name ? fn->name : "?");
        return v_unit();
    }

    Env *local = env_new(in, NULL); /* functions do not close over the caller */

    /* `this` and the receiver's fields are visible inside a method body. */
    if (self) {
        Value tv;
        tv.kind = V_OBJECT;
        tv.as.o = self;
        env_define(in, local, "this", tv);
        for (size_t i = 0; i < self->names.len; i++) {
            sn_list_push(in->arena, &local->names, self->names.items[i]);
            sn_list_push(in->arena, &local->slots, self->slots.items[i]);
        }
    }

    for (size_t i = 0; i < fn->params.len; i++) {
        const SnParam *p = (const SnParam *)fn->params.items[i];
        Value v;
        if (args && i < args->len) {
            v = eval_expr(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = eval_expr(in, caller, p->def);
        } else {
            v = default_for(p->type);
        }
        env_define(in, local, p->name, v);
    }

    in->ret = v_unit();
    Flow f = exec_stmt(in, local, fn->body);
    Value r = (f == FLOW_RETURN) ? in->ret : v_unit();
    in->flow = FLOW_NORMAL;
    return r;
}

static Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                         Env *caller, SnSpan span) {
    return call_function(in, m, args, caller, self, span);
}

/* Console.print / Console.printline — the only intrinsics wired up so far.
 * Returns 1 when the call was an intrinsic and `out` was set. */
static int try_intrinsic(Interp *in, Env *env, const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    if (!callee || callee->kind != SN_EXPR_MEMBER || !callee->lhs ||
        callee->lhs->kind != SN_EXPR_IDENT || !callee->lhs->text) {
        return 0;
    }
    if (strcmp(callee->lhs->text, "Console") != 0) {
        return 0;
    }

    int newline;
    if (strcmp(callee->text, "printline") == 0)      newline = 1;
    else if (strcmp(callee->text, "print") == 0)     newline = 0;
    else if (strcmp(callee->text, "err") == 0)       newline = 2;
    else if (strcmp(callee->text, "warn") == 0)      newline = 2;
    else {
        rt_error(in, SNOVA_UNKNOWN_INTRINSIC, call->span,
                 "Console.%s is not implemented yet", callee->text);
        *out = v_unit();
        return 1;
    }

    const char *text = "";
    if (call->args.len > 0) {
        Value v = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
        text = to_string(in, v, call->span);
    }
    FILE *dst = (newline == 2) ? stderr : stdout;
    fputs(text, dst);
    if (newline) {
        fputc('\n', dst);
    }
    fflush(dst);
    *out = v_unit();
    return 1;
}

/* ── expressions ──────────────────────────────────────────────────────────── */

static long long as_int(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_INT:  return v.as.i;
    case V_BOOL: return v.as.b;
    case V_DOUBLE: return (long long)v.as.d;
    default:
        rt_error(in, SNOVA_TYPE_ERROR, span, "expected a number");
        return 0;
    }
}

static int truthy(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_BOOL: return v.as.b;
    case V_INT:  return v.as.i != 0;
    default:
        rt_error(in, SNOVA_TYPE_ERROR, span, "expected a bool condition");
        return 0;
    }
}

static Value eval_binary(Interp *in, Env *env, const SnExpr *e) {
    Value a = eval_expr(in, env, e->lhs);

    /* Short-circuit before evaluating the right operand. */
    if (e->op == SN_TOK_ANDAND) {
        if (!truthy(in, a, e->span)) return v_bool(0);
        return v_bool(truthy(in, eval_expr(in, env, e->rhs), e->span));
    }
    if (e->op == SN_TOK_OROR) {
        if (truthy(in, a, e->span)) return v_bool(1);
        return v_bool(truthy(in, eval_expr(in, env, e->rhs), e->span));
    }

    Value b = eval_expr(in, env, e->rhs);

    /* `+` concatenates when either side is a string. */
    if (e->op == SN_TOK_PLUS && (a.kind == V_STRING || b.kind == V_STRING)) {
        return v_str(str_concat(in, to_string(in, a, e->span),
                                to_string(in, b, e->span)));
    }

    if (a.kind == V_DOUBLE || b.kind == V_DOUBLE) {
        double x = (a.kind == V_DOUBLE) ? a.as.d : (double)as_int(in, a, e->span);
        double y = (b.kind == V_DOUBLE) ? b.as.d : (double)as_int(in, b, e->span);
        switch (e->op) {
        case SN_TOK_PLUS:  return v_double(x + y);
        case SN_TOK_MINUS: return v_double(x - y);
        case SN_TOK_STAR:  return v_double(x * y);
        case SN_TOK_SLASH: return v_double(y == 0 ? 0 : x / y);
        case SN_TOK_LT: return v_bool(x < y);
        case SN_TOK_GT: return v_bool(x > y);
        case SN_TOK_LE: return v_bool(x <= y);
        case SN_TOK_GE: return v_bool(x >= y);
        case SN_TOK_EQ: return v_bool(x == y);
        case SN_TOK_NE: return v_bool(x != y);
        default: break;
        }
    }

    if (a.kind == V_STRING && b.kind == V_STRING) {
        int c = strcmp(a.as.s, b.as.s);
        switch (e->op) {
        case SN_TOK_EQ: return v_bool(c == 0);
        case SN_TOK_NE: return v_bool(c != 0);
        case SN_TOK_LT: return v_bool(c < 0);
        case SN_TOK_GT: return v_bool(c > 0);
        default: break;
        }
    }

    long long x = as_int(in, a, e->span);
    long long y = as_int(in, b, e->span);
    switch (e->op) {
    case SN_TOK_PLUS:    return v_int(x + y);
    case SN_TOK_MINUS:   return v_int(x - y);
    case SN_TOK_STAR:    return v_int(x * y);
    case SN_TOK_SLASH:
        if (y == 0) { rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero"); return v_int(0); }
        return v_int(x / y);
    case SN_TOK_PERCENT:
        if (y == 0) { rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero"); return v_int(0); }
        return v_int(x % y);
    case SN_TOK_LT: return v_bool(x < y);
    case SN_TOK_GT: return v_bool(x > y);
    case SN_TOK_LE: return v_bool(x <= y);
    case SN_TOK_GE: return v_bool(x >= y);
    case SN_TOK_EQ: return v_bool(x == y);
    case SN_TOK_NE: return v_bool(x != y);
    default:
        rt_error(in, SNOVA_UNSUPPORTED, e->span, "operator not supported yet");
        return v_unit();
    }
}

static Value eval_call(Interp *in, Env *env, const SnExpr *e) {
    Value out;
    if (try_intrinsic(in, env, e, &out)) {
        return out;
    }

    const SnExpr *callee = e->lhs;
    if (!callee) {
        rt_error(in, SNOVA_NOT_CALLABLE, e->span, "call target is not an expression");
        return v_unit();
    }

    /* Bare name: a top-level function, or a constructor `Counter()`. */
    if (callee->kind == SN_EXPR_IDENT && callee->text) {
        const SnDecl *fn = find_top(in, callee->text, SN_DECL_FUNC);
        if (fn) {
            return call_function(in, fn, (SnList *)&e->args, env, NULL, e->span);
        }
        const SnDecl *cls = find_type(in, callee->text);
        if (cls) {
            Value v;
            v.kind = V_OBJECT;
            v.as.o = instantiate(in, cls, (SnList *)&e->args, env, e->span);
            return v;
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown function `%s`",
                 callee->text);
        return v_unit();
    }

    /* Method call on a receiver. */
    if (callee->kind == SN_EXPR_MEMBER && callee->lhs) {
        Value recv = eval_expr(in, env, callee->lhs);
        if (recv.kind == V_OBJECT) {
            const SnDecl *m = find_member(recv.as.o->cls, callee->text);
            if (m) {
                return call_method(in, recv.as.o, m, (SnList *)&e->args, env, e->span);
            }
        }
        /* Static method: `Foo.bar()` where Foo is a declared type. */
        if (callee->lhs->kind == SN_EXPR_IDENT && callee->lhs->text) {
            const SnDecl *cls = find_type(in, callee->lhs->text);
            const SnDecl *m = find_member(cls, callee->text);
            if (m) {
                return call_function(in, m, (SnList *)&e->args, env, NULL, e->span);
            }
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown method `%s`",
                 callee->text ? callee->text : "?");
        return v_unit();
    }

    rt_error(in, SNOVA_NOT_CALLABLE, e->span, "expression is not callable");
    return v_unit();
}

static Value eval_expr(Interp *in, Env *env, const SnExpr *e) {
    if (!e || in->failed) {
        return v_unit();
    }

    switch (e->kind) {
    case SN_EXPR_INT:
    case SN_EXPR_LONG:
        return v_int(strtoll(e->text, NULL, 0));
    case SN_EXPR_DOUBLE:
    case SN_EXPR_DECIMAL:
        return v_double(strtod(e->text, NULL));
    case SN_EXPR_BOOL:
        return v_bool(strcmp(e->text, "true") == 0);
    case SN_EXPR_STRING:
        return v_str(decode_string(in, env, e));
    case SN_EXPR_CHAR:
        return v_str(e->text);
    case SN_EXPR_THIS: {
        Value *v = env_lookup(env, "this");
        return v ? *v : v_unit();
    }
    case SN_EXPR_IDENT: {
        Value *v = env_lookup(env, e->text);
        if (v) {
            return *v;
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "undefined name `%s`", e->text);
        return v_unit();
    }
    case SN_EXPR_MEMBER: {
        Value recv = eval_expr(in, env, e->lhs);
        if (recv.kind == V_OBJECT) {
            Value *f = object_field(recv.as.o, e->text);
            if (f) {
                return *f;
            }
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "no field `%s`", e->text);
        return v_unit();
    }
    case SN_EXPR_CALL:
        return eval_call(in, env, e);
    case SN_EXPR_UNARY: {
        if (e->op == SN_TOK_BANG) {
            return v_bool(!truthy(in, eval_expr(in, env, e->lhs), e->span));
        }
        if (e->op == SN_TOK_MINUS) {
            Value v = eval_expr(in, env, e->lhs);
            if (v.kind == V_DOUBLE) return v_double(-v.as.d);
            return v_int(-as_int(in, v, e->span));
        }
        if (e->op == SN_TOK_PLUS) {
            return eval_expr(in, env, e->lhs);
        }
        rt_error(in, SNOVA_UNSUPPORTED, e->span, "unary operator not supported yet");
        return v_unit();
    }
    case SN_EXPR_BINARY:
        return eval_binary(in, env, e);
    case SN_EXPR_ASSIGN: {
        Value val = eval_expr(in, env, e->rhs);
        Value *slot = NULL;
        if (e->lhs->kind == SN_EXPR_IDENT) {
            slot = env_lookup(env, e->lhs->text);
        } else if (e->lhs->kind == SN_EXPR_MEMBER) {
            Value recv = eval_expr(in, env, e->lhs->lhs);
            if (recv.kind == V_OBJECT) {
                slot = object_field(recv.as.o, e->lhs->text);
            }
        }
        if (!slot) {
            rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "cannot assign to this target");
            return v_unit();
        }
        if (e->op != SN_TOK_ASSIGN) {
            SnExpr tmp = *e;
            tmp.kind = SN_EXPR_BINARY;
            switch (e->op) {
            case SN_TOK_PLUS_EQ:  tmp.op = SN_TOK_PLUS;  break;
            case SN_TOK_MINUS_EQ: tmp.op = SN_TOK_MINUS; break;
            case SN_TOK_STAR_EQ:  tmp.op = SN_TOK_STAR;  break;
            case SN_TOK_SLASH_EQ: tmp.op = SN_TOK_SLASH; break;
            default: tmp.op = SN_TOK_PLUS; break;
            }
            val = eval_binary(in, env, &tmp);
        }
        *slot = val;
        return val;
    }
    default:
        rt_error(in, SNOVA_UNSUPPORTED, e->span,
                 "this expression form is not executable yet");
        return v_unit();
    }
}

/* ── statements ───────────────────────────────────────────────────────────── */

static Flow exec_stmt(Interp *in, Env *env, const SnStmt *s) {
    if (!s || in->failed) {
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }

    switch (s->kind) {
    case SN_STMT_BLOCK: {
        Env *inner = env_new(in, env);
        for (size_t i = 0; i < s->stmts.len; i++) {
            Flow f = exec_stmt(in, inner, (const SnStmt *)s->stmts.items[i]);
            if (f != FLOW_NORMAL) {
                return f;
            }
        }
        return FLOW_NORMAL;
    }
    case SN_STMT_LET:
    case SN_STMT_VAR: {
        Value v = s->expr ? eval_expr(in, env, s->expr) : default_for(s->type);
        env_define(in, env, s->name, v);
        return FLOW_NORMAL;
    }
    case SN_STMT_EXPR:
        eval_expr(in, env, s->expr);
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    case SN_STMT_RETURN:
        in->ret = s->expr ? eval_expr(in, env, s->expr) : v_unit();
        return FLOW_RETURN;
    case SN_STMT_IF:
        if (truthy(in, eval_expr(in, env, s->expr), s->span)) {
            return exec_stmt(in, env, s->then_br);
        }
        return s->else_br ? exec_stmt(in, env, s->else_br) : FLOW_NORMAL;
    case SN_STMT_WHILE: {
        int guard = 0;
        while (!in->failed && truthy(in, eval_expr(in, env, s->expr), s->span)) {
            Flow f = exec_stmt(in, env, s->then_br);
            if (f == FLOW_RETURN) return f;
            if (f == FLOW_BREAK) break;
            if (++guard > 100000000) {
                rt_error(in, SNOVA_UNSUPPORTED, s->span, "loop exceeded iteration guard");
                break;
            }
        }
        return FLOW_NORMAL;
    }
    case SN_STMT_BREAK:    return FLOW_BREAK;
    case SN_STMT_CONTINUE: return FLOW_CONTINUE;
    default:
        rt_error(in, SNOVA_UNSUPPORTED, s->span,
                 "this statement form is not executable yet");
        return FLOW_RETURN;
    }
}

/* ── string decoding + interpolation ──────────────────────────────────────── */

/* Interpolation needs to evaluate an expression written inside a literal. The
 * inner source is lexed and parsed on demand with the ordinary front-end — no
 * ad-hoc string scanning, so `"${a + b}"` and `"${f(x)}"` behave exactly like
 * the same code written outside the string. */
static Value eval_interp_source(Interp *in, Env *env, const char *src, size_t len,
                                SnSpan span) {
    char *buf = sn_arena_strndup(in->arena, src, len);
    SnTokenVec toks;
    if (sn_lex(in->arena, in->diag, buf, len, &toks) != 0) {
        return v_unit();
    }
    SnExpr *e = sn_parse_expr_only(in->arena, in->diag, &toks);
    if (!e) {
        rt_error(in, SNOVA_UNSUPPORTED, span,
                 "could not parse the expression inside `${...}`");
        return v_unit();
    }
    return eval_expr(in, env, e);
}

static const char *decode_string(Interp *in, Env *env, const SnExpr *e) {
    const char *raw = e->text;
    size_t n = strlen(raw);
    if (n >= 2 && raw[0] == '"') {
        raw++;
        n -= 2;
    }

    size_t cap = n + 1;
    char *out = (char *)sn_arena_alloc(in->arena, cap);
    size_t w = 0;

#define PUSH(c) do { if (w + 1 < cap) out[w++] = (c); } while (0)
#define PUSHS(s) do { const char *_p = (s); while (*_p) { \
        if (w + 1 >= cap) { size_t nc = cap * 2 + strlen(_p); \
            char *nb = (char *)sn_arena_alloc(in->arena, nc); \
            memcpy(nb, out, w); out = nb; cap = nc; } \
        out[w++] = *_p++; } } while (0)

    for (size_t i = 0; i < n; i++) {
        char c = raw[i];
        if (c == '\\' && i + 1 < n) {
            char d = raw[++i];
            switch (d) {
            case 'n': PUSH('\n'); break;
            case 't': PUSH('\t'); break;
            case 'r': PUSH('\r'); break;
            case '0': PUSH('\0'); break;
            case '\\': PUSH('\\'); break;
            case '"': PUSH('"'); break;
            default: PUSH(d); break;
            }
            continue;
        }
        if (c == '$' && i + 1 < n && raw[i + 1] == '$') {
            PUSH('$');
            i++;
            continue;
        }
        if (c == '$' && i + 1 < n && raw[i + 1] == '{') {
            size_t start = i + 2;
            int depth = 1;
            size_t j = start;
            for (; j < n && depth > 0; j++) {
                if (raw[j] == '{') depth++;
                else if (raw[j] == '}') depth--;
                if (depth == 0) break;
            }
            Value v = eval_interp_source(in, env, raw + start, j - start, e->span);
            PUSHS(to_string(in, v, e->span));
            i = j;
            continue;
        }
        if (w + 1 >= cap) {
            size_t nc = cap * 2;
            char *nb = (char *)sn_arena_alloc(in->arena, nc);
            memcpy(nb, out, w);
            out = nb;
            cap = nc;
        }
        out[w++] = c;
    }
#undef PUSH
#undef PUSHS
    out[w] = '\0';
    return out;
}

int sn_eval_run(SnArena *arena, SnDiagSink *diag, const SnUnit *unit) {
    Interp in;
    in.arena = arena;
    in.diag = diag;
    in.unit = unit;
    in.ret = v_unit();
    in.flow = FLOW_NORMAL;
    in.failed = 0;

    const SnDecl *main_fn = find_top(&in, "main", SN_DECL_FUNC);
    if (!main_fn) {
        SnSpan z = {0, 0, 1, 1};
        sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_NO_MAIN, z,
                     "no `func main()` found in this file");
        return -1;
    }

    Env *global = env_new(&in, NULL);
    SnList no_args = {0};
    Value r = call_function(&in, main_fn, &no_args, global, NULL, main_fn->span);

    if (in.failed) {
        return -1;
    }
    return (r.kind == V_INT) ? (int)r.as.i : 0;
}
