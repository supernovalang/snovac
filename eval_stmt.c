/* eval_stmt.c — statement execution and control flow. */
#include "eval_internal.h"

/* A runaway loop in a smoke-path interpreter should report, not hang. */
#define SN_LOOP_GUARD 100000000

static Flow exec_block(Interp *in, Env *env, const SnStmt *s) {
    Env *inner = env_new(in, env);
    for (size_t i = 0; i < s->stmts.len; i++) {
        Flow f = exec_stmt(in, inner, (const SnStmt *)s->stmts.items[i]);
        if (f != FLOW_NORMAL) {
            return f;
        }
    }
    return FLOW_NORMAL;
}

static Flow exec_while(Interp *in, Env *env, const SnStmt *s) {
    int guard = 0;
    while (!in->failed && truthy(in, eval_expr(in, env, s->expr), s->span)) {
        Flow f = exec_stmt(in, env, s->then_br);
        if (f == FLOW_RETURN) {
            return f;
        }
        if (f == FLOW_BREAK) {
            break;
        }
        if (++guard > SN_LOOP_GUARD) {
            rt_error(in, SNOVA_UNSUPPORTED, s->span,
                     "loop exceeded iteration guard");
            break;
        }
    }
    return FLOW_NORMAL;
}

/* Bare variant name of a possibly qualified pattern path: `Option.Some` and
 * `Some` both match a variant constructed as `Some`. */
static const char *pattern_variant_name(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : path;
}

int pattern_match_bind(Interp *in, Env *env, const SnPattern *pat,
                       Value subject) {
    switch (pat->kind) {
    case SN_PAT_WILDCARD:
        return 1;
    case SN_PAT_BINDING:
        /* A bare pattern name is ambiguous until it's checked against
         * declared variants (parse_pattern() in parse_type.c records it as
         * BINDING and defers the call here). A payload-less variant name
         * ("Draft", "None") must match by identity, not swallow every
         * subject as a fresh local. */
        if (is_variant_constructor(in, pat->name)) {
            return subject.kind == V_VARIANT &&
                  subject.as.vt->payload.len == 0 &&
                  strcmp(subject.as.vt->name, pat->name) == 0;
        }
        env_define(in, env, pat->name, subject);
        return 1;
    case SN_PAT_LITERAL: {
        Value lit = eval_expr(in, env, pat->literal);
        return !in->failed && value_equals(lit, subject);
    }
    case SN_PAT_VARIANT: {
        if (subject.kind != V_VARIANT) {
            return 0;
        }
        if (strcmp(pattern_variant_name(pat->name), subject.as.vt->name) != 0) {
            return 0;
        }
        if (pat->subs.len > subject.as.vt->payload.len) {
            return 0;
        }
        for (size_t i = 0; i < pat->subs.len; i++) {
            const Value *payload = (const Value *)subject.as.vt->payload.items[i];
            if (!pattern_match_bind(in, env, (const SnPattern *)pat->subs.items[i],
                                    *payload)) {
                return 0;
            }
        }
        return 1;
    }
    }
    return 0;
}

const SnMatchArm *match_select_arm(Interp *in, Env *env, const SnList *arms,
                                   Value subject, Env **arm_env) {
    for (size_t i = 0; i < arms->len && !in->failed; i++) {
        const SnMatchArm *arm = (const SnMatchArm *)arms->items[i];
        Env *candidate = env_new(in, env);
        if (!pattern_match_bind(in, candidate, arm->pattern, subject)) {
            continue;
        }
        if (arm->guard &&
            !truthy(in, eval_expr(in, candidate, arm->guard), arm->span)) {
            continue;
        }
        *arm_env = candidate;
        return arm;
    }
    *arm_env = NULL;
    return NULL;
}

static Flow exec_match(Interp *in, Env *env, const SnStmt *s) {
    Value subject = eval_expr(in, env, s->expr);
    if (in->failed) {
        return FLOW_RETURN;
    }
    Env *arm_env = NULL;
    const SnMatchArm *arm = match_select_arm(in, env, &s->arms, subject, &arm_env);
    if (!arm) {
        /* A statement-position match with no matching arm is a no-op, mirroring
         * an `if` chain whose conditions are all false. */
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }
    if (arm->body) {
        return exec_stmt(in, arm_env, arm->body);
    }
    if (arm->value) {
        eval_expr(in, arm_env, arm->value);
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }
    return FLOW_NORMAL;
}

static Flow exec_for(Interp *in, Env *env, const SnStmt *s) {
    if (!s->expr) return FLOW_NORMAL;
    Value iterable = eval_expr(in, env, s->expr);
    if (in->failed) return FLOW_RETURN;

    if (iterable.kind == V_INT) {
        long long limit = iterable.as.i;
        int guard = 0;
        for (long long i = 0; i < limit && !in->failed; i++) {
            Env *loop_env = env_new(in, env);
            if (s->name) env_define(in, loop_env, s->name, v_int(i));
            Flow f = exec_stmt(in, loop_env, s->then_br);
            if (f == FLOW_RETURN) return f;
            if (f == FLOW_BREAK) break;
            if (++guard > SN_LOOP_GUARD) {
                rt_error(in, SNOVA_UNSUPPORTED, s->span, "loop exceeded iteration guard");
                break;
            }
        }
    } else if (iterable.kind == V_ARRAY) {
        int guard = 0;
        for (size_t i = 0; i < iterable.as.arr->items.len && !in->failed; i++) {
            Value *item = (Value *)iterable.as.arr->items.items[i];
            Env *loop_env = env_new(in, env);
            if (s->name) env_define(in, loop_env, s->name, *item);
            Flow f = exec_stmt(in, loop_env, s->then_br);
            if (f == FLOW_RETURN) return f;
            if (f == FLOW_BREAK) break;
            if (++guard > SN_LOOP_GUARD) {
                rt_error(in, SNOVA_UNSUPPORTED, s->span, "loop exceeded iteration guard");
                break;
            }
        }
    }
    return FLOW_NORMAL;
}

Flow exec_stmt(Interp *in, Env *env, const SnStmt *s) {
    if (!s || in->failed) {
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }

    switch (s->kind) {
    case SN_STMT_BLOCK:
        return exec_block(in, env, s);

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

    case SN_STMT_WHILE:
        return exec_while(in, env, s);

    case SN_STMT_FOR:
        return exec_for(in, env, s);

    case SN_STMT_MATCH:
        return exec_match(in, env, s);

    case SN_STMT_BREAK:    return FLOW_BREAK;
    case SN_STMT_CONTINUE: return FLOW_CONTINUE;

    default:
        rt_error(in, SNOVA_UNSUPPORTED, s->span,
                 "this statement form is not executable yet");
        return FLOW_RETURN;
    }
}
