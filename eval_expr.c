/* eval_expr.c — expression evaluation: coercions, binary operators, calls and
 * the intrinsic bridge. */
#include "eval_internal.h"

long long as_int(Interp *in, Value v, SnSpan span) {
    switch (v.kind) {
    case V_INT:    return v.as.i;
    case V_BOOL:   return v.as.b;
    case V_DOUBLE: return (long long)v.as.d;
    default:
        rt_error(in, SNOVA_TYPE_ERROR, span, "expected a number");
        return 0;
    }
}

int truthy(Interp *in, Value v, SnSpan span) {
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
    case SN_TOK_PLUS:  return v_int(x + y);
    case SN_TOK_MINUS: return v_int(x - y);
    case SN_TOK_STAR:  return v_int(x * y);
    case SN_TOK_SLASH:
        if (y == 0) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero");
            return v_int(0);
        }
        return v_int(x / y);
    case SN_TOK_PERCENT:
        if (y == 0) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span, "division by zero");
            return v_int(0);
        }
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
    if (strcmp(callee->text, "printline") == 0)  newline = 1;
    else if (strcmp(callee->text, "print") == 0) newline = 0;
    else if (strcmp(callee->text, "err") == 0)   newline = 2;
    else if (strcmp(callee->text, "warn") == 0)  newline = 2;
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

static Value eval_call(Interp *in, Env *env, const SnExpr *e) {
    Value out;
    if (try_intrinsic(in, env, e, &out)) {
        return out;
    }

    const SnExpr *callee = e->lhs;
    if (!callee) {
        rt_error(in, SNOVA_NOT_CALLABLE, e->span,
                 "call target is not an expression");
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
                return call_method(in, recv.as.o, m, (SnList *)&e->args, env,
                                   e->span);
            }
        }
        /* Static method: `Foo.bar()` where Foo is a declared type. */
        if (callee->lhs->kind == SN_EXPR_IDENT && callee->lhs->text) {
            const SnDecl *cls = find_type(in, callee->lhs->text);
            const SnDecl *m = find_member(cls, callee->text);
            if (m) {
                return call_function(in, m, (SnList *)&e->args, env, NULL,
                                     e->span);
            }
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown method `%s`",
                 callee->text ? callee->text : "?");
        return v_unit();
    }

    rt_error(in, SNOVA_NOT_CALLABLE, e->span, "expression is not callable");
    return v_unit();
}

static Value eval_unary(Interp *in, Env *env, const SnExpr *e) {
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

static Value eval_assign(Interp *in, Env *env, const SnExpr *e) {
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
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                 "cannot assign to this target");
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

Value eval_expr(Interp *in, Env *env, const SnExpr *e) {
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
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "undefined name `%s`",
                 e->text);
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
    case SN_EXPR_UNARY:
        return eval_unary(in, env, e);
    case SN_EXPR_BINARY:
        return eval_binary(in, env, e);
    case SN_EXPR_ASSIGN:
        return eval_assign(in, env, e);
    default:
        rt_error(in, SNOVA_UNSUPPORTED, e->span,
                 "this expression form is not executable yet");
        return v_unit();
    }
}
