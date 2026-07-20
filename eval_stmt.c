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

    case SN_STMT_BREAK:    return FLOW_BREAK;
    case SN_STMT_CONTINUE: return FLOW_CONTINUE;

    default:
        rt_error(in, SNOVA_UNSUPPORTED, s->span,
                 "this statement form is not executable yet");
        return FLOW_RETURN;
    }
}
