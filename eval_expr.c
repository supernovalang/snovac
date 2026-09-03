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

    if (e->op == SN_TOK_EQ) {
        return v_bool(value_equals(a, b));
    }
    if (e->op == SN_TOK_NE) {
        return v_bool(!value_equals(a, b));
    }

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
    case SN_TOK_AMP:    return v_int(x & y);
    case SN_TOK_PIPE:   return v_int(x | y);
    case SN_TOK_CARET:  return v_int(x ^ y);
    case SN_TOK_SHL:    return v_int(x << y);
    case SN_TOK_SHR:    return v_int(x >> y);
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

/* Builds a variant value from a constructor call's evaluated arguments. */
static Value make_variant_from_args(Interp *in, Env *env, const char *name,
                                    const SnList *args) {
    SnList payload = {0};
    for (size_t i = 0; i < args->len; i++) {
        Value v = eval_expr(in, env, (const SnExpr *)args->items[i]);
        Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
        *slot = v;
        sn_list_push(in->arena, &payload, slot);
    }
    return v_variant(in, name, payload);
}

/* `Some` / `Ok` / `Err` are built-in variant constructors; any other
 * capitalized name is looked up among declared enums' variants so user enums
 * construct the same way. */
int is_variant_constructor(Interp *in, const char *name) {
    const char *vname = strrchr(name, '.') ? strrchr(name, '.') + 1 : name;
    if (strcmp(vname, "Some") == 0 || strcmp(vname, "Ok") == 0 ||
        strcmp(vname, "Err") == 0 || strcmp(vname, "None") == 0) {
        return 1;
    }
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if (d->kind != SN_DECL_ENUM) {
            continue;
        }
        for (size_t j = 0; j < d->variants.len; j++) {
            const SnDecl *v = (const SnDecl *)d->variants.items[j];
            if (v->name && strcmp(v->name, vname) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Calls a single-parameter lambda with an already-evaluated value. */
static Value call_lambda_with_value(Interp *in, const LambdaVal *lam,
                                    Value arg, SnSpan span) {
    (void)span;
    Env *local = env_new(in, lam->env);
    if (lam->expr->params.len > 0) {
        const SnParam *p = (const SnParam *)lam->expr->params.items[0];
        env_define(in, local, p->name, arg);
    }
    if (lam->expr->value) {
        return eval_expr(in, local, lam->expr->value);
    }
    if (lam->expr->body) {
        Value saved = in->ret;
        in->ret = v_unit();
        Flow f = exec_stmt(in, local, lam->expr->body);
        Value r = (f == FLOW_RETURN) ? in->ret : v_unit();
        in->ret = saved;
        in->flow = FLOW_NORMAL;
        return r;
    }
    return v_unit();
}

static int try_array_method(Interp *in, Env *env, Value recv,
                            const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    if (!recv.as.arr) {
        return 0;
    }
    if (strcmp(m, "len") == 0 || strcmp(m, "length") == 0) {
        *out = v_int((long long)recv.as.arr->items.len);
        return 1;
    }
    if (strcmp(m, "isEmpty") == 0) {
        *out = v_bool(recv.as.arr->items.len == 0);
        return 1;
    }
    if (strcmp(m, "push") == 0) {
        if (call->args.len > 0) {
            Value item = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            Value *boxed = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
            *boxed = item;
            sn_list_push(in->arena, &recv.as.arr->items, boxed);
        }
        *out = v_int((long long)recv.as.arr->items.len);
        return 1;
    }
    if (strcmp(m, "pop") == 0) {
        if (recv.as.arr->items.len > 0) {
            Value *last = (Value *)recv.as.arr->items.items[recv.as.arr->items.len - 1];
            recv.as.arr->items.len--;
            *out = *last;
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "get") == 0) {
        if (call->args.len > 0) {
            long long idx = as_int(in, eval_expr(in, env, (const SnExpr *)call->args.items[0]), call->span);
            if (idx >= 0 && (size_t)idx < recv.as.arr->items.len) {
                Value *elem = (Value *)recv.as.arr->items.items[idx];
                *out = *elem;
            } else {
                *out = v_unit();
            }
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "clear") == 0) {
        recv.as.arr->items.len = 0;
        *out = v_int(0);
        return 1;
    }
    return 0;
}

static int try_string_method(Interp *in, Env *env, Value recv,
                             const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    const char *s = recv.as.s ? recv.as.s : "";
    if (strcmp(m, "toUpper") == 0) {
        size_t len = strlen(s);
        char *buf = (char *)sn_arena_alloc(in->arena, len + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)s[i]);
        buf[len] = '\0';
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "toLower") == 0) {
        size_t len = strlen(s);
        char *buf = (char *)sn_arena_alloc(in->arena, len + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)s[i]);
        buf[len] = '\0';
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "len") == 0 || strcmp(m, "length") == 0) {
        *out = v_int((long long)strlen(s));
        return 1;
    }
    if (strcmp(m, "isEmpty") == 0) {
        *out = v_bool(strlen(s) == 0);
        return 1;
    }
    if (strcmp(m, "trim") == 0) {
        size_t len = strlen(s);
        size_t start = 0;
        while (start < len && isspace((unsigned char)s[start])) start++;
        size_t end = len;
        while (end > start && isspace((unsigned char)s[end - 1])) end--;
        char *buf = sn_arena_strndup(in->arena, s + start, end - start);
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "startsWith") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *prefix = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
            size_t plen = strlen(prefix);
            *out = v_bool(strncmp(s, prefix, plen) == 0);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "endsWith") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *suffix = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
            size_t slen = strlen(s);
            size_t suflen = strlen(suffix);
            *out = v_bool(slen >= suflen && strcmp(s + (slen - suflen), suffix) == 0);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "contains") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *sub = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
            *out = v_bool(strstr(s, sub) != NULL);
        } else {
            *out = v_bool(0);
        }
        return 1;
    }
    if (strcmp(m, "indexOf") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            const char *sub = (arg.kind == V_STRING && arg.as.s) ? arg.as.s : "";
            char *p = strstr((char *)s, sub);
            if (p) {
                *out = v_int((long long)(p - s));
            } else {
                *out = v_int(-1);
            }
        } else {
            *out = v_int(-1);
        }
        return 1;
    }
    if (strcmp(m, "charAt") == 0) {
        if (call->args.len > 0) {
            Value arg = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            long long idx = as_int(in, arg, call->span);
            size_t slen = strlen(s);
            if (idx >= 0 && (size_t)idx < slen) {
                char chbuf[2] = { s[idx], '\0' };
                Value chVal = v_str(sn_arena_strndup(in->arena, chbuf, 1));
                SnList payload = {0};
                Value *boxed = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
                *boxed = chVal;
                sn_list_push(in->arena, &payload, boxed);
                *out = v_variant(in, "Some", payload);
            } else {
                SnList empty = {0};
                *out = v_variant(in, "None", empty);
            }
        } else {
            SnList empty = {0};
            *out = v_variant(in, "None", empty);
        }
        return 1;
    }
    if (strcmp(m, "substring") == 0) {
        size_t slen = strlen(s);
        long long start = 0;
        long long end = (long long)slen;
        if (call->args.len > 0) {
            start = as_int(in, eval_expr(in, env, (const SnExpr *)call->args.items[0]), call->span);
            if (start < 0) start = 0;
            if ((size_t)start > slen) start = (long long)slen;
        }
        if (call->args.len > 1) {
            end = as_int(in, eval_expr(in, env, (const SnExpr *)call->args.items[1]), call->span);
            if (end < start) end = start;
            if ((size_t)end > slen) end = (long long)slen;
        }
        char *buf = sn_arena_strndup(in->arena, s + start, (size_t)(end - start));
        *out = v_str(buf);
        return 1;
    }
    if (strcmp(m, "replaceFirst") == 0) {
        if (call->args.len >= 2) {
            Value target = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            Value repl = eval_expr(in, env, (const SnExpr *)call->args.items[1]);
            const char *tstr = (target.kind == V_STRING && target.as.s) ? target.as.s : "";
            const char *rstr = (repl.kind == V_STRING && repl.as.s) ? repl.as.s : "";
            char *found = strstr((char *)s, tstr);
            if (found && tstr[0]) {
                size_t prefix_len = (size_t)(found - s);
                size_t tlen = strlen(tstr);
                size_t rlen = strlen(rstr);
                size_t rest_len = strlen(found + tlen);
                char *buf = (char *)sn_arena_alloc(in->arena, prefix_len + rlen + rest_len + 1);
                memcpy(buf, s, prefix_len);
                memcpy(buf + prefix_len, rstr, rlen);
                memcpy(buf + prefix_len + rlen, found + tlen, rest_len + 1);
                *out = v_str(buf);
            } else {
                *out = v_str(s);
            }
        } else {
            *out = v_str(s);
        }
        return 1;
    }
    return 0;
}

/* Option/Result (and general variant) instance methods: the functional
 * unwrapping surface — `isSome`, `unwrap`, `unwrapOr`, `unwrapOrElse`,
 * `unwrapTo`, `unwrapIf`, `map`. Returns 1 when handled. */
static int try_variant_method(Interp *in, Env *env, Value recv,
                              const SnExpr *call, Value *out) {
    const SnExpr *callee = call->lhs;
    const char *m = callee->text;
    const VariantVal *vt = recv.as.vt;
    int has_payload = vt->payload.len > 0;
    int is_ok_like = strcmp(vt->name, "Some") == 0 || strcmp(vt->name, "Ok") == 0;
    Value payload = has_payload ? *(const Value *)vt->payload.items[0] : v_unit();

    if (strcmp(m, "isSome") == 0) { *out = v_bool(strcmp(vt->name, "Some") == 0); return 1; }
    if (strcmp(m, "isNone") == 0) { *out = v_bool(strcmp(vt->name, "None") == 0); return 1; }
    if (strcmp(m, "isOk") == 0)   { *out = v_bool(strcmp(vt->name, "Ok") == 0); return 1; }
    if (strcmp(m, "isErr") == 0)  { *out = v_bool(strcmp(vt->name, "Err") == 0); return 1; }

    if (strcmp(m, "unwrap") == 0) {
        if (!is_ok_like) {
            rt_error(in, SNOVA_TYPE_ERROR, call->span,
                     "called `unwrap` on `%s`", vt->name);
            *out = v_unit();
            return 1;
        }
        *out = payload;
        return 1;
    }
    if (strcmp(m, "unwrapOr") == 0) {
        if (is_ok_like) {
            *out = payload;
        } else if (call->args.len > 0) {
            *out = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
        } else {
            *out = v_unit();
        }
        return 1;
    }
    if (strcmp(m, "unwrapOrElse") == 0) {
        if (is_ok_like) {
            *out = payload;
            return 1;
        }
        if (call->args.len > 0) {
            Value fn = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                SnList no_args = {0};
                *out = call_lambda(in, fn.as.lam, &no_args, env, call->span);
                return 1;
            }
            *out = fn;
            return 1;
        }
        *out = v_unit();
        return 1;
    }
    if (strcmp(m, "unwrapTo") == 0) {
        /* `.unwrapTo(Type)` — unwrap asserting the payload's declared type.
         * The argument is a bare type name, so it is validated against the
         * payload instead of being evaluated as an expression. */
        if (!is_ok_like) {
            rt_error(in, SNOVA_TYPE_ERROR, call->span,
                     "called `unwrapTo` on `%s`", vt->name);
            *out = v_unit();
            return 1;
        }
        if (call->args.len > 0) {
            const SnExpr *arg = (const SnExpr *)call->args.items[0];
            if (arg->kind == SN_EXPR_IDENT && arg->text &&
                payload.kind == V_OBJECT && payload.as.o->cls->name &&
                strcmp(payload.as.o->cls->name, arg->text) != 0) {
                rt_error(in, SNOVA_TYPE_ERROR, call->span,
                         "`unwrapTo(%s)` does not match payload of type `%s`",
                         arg->text, payload.as.o->cls->name);
                *out = v_unit();
                return 1;
            }
        }
        *out = payload;
        return 1;
    }
    if (strcmp(m, "unwrapIf") == 0) {
        /* `.unwrapIf((entry) -> { ... })` — run the lambda with the payload
         * when present; the original value is returned so calls chain. */
        if (is_ok_like && call->args.len > 0) {
            Value fn = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                (void)call_lambda_with_value(in, fn.as.lam, payload, call->span);
            }
        }
        *out = recv;
        return 1;
    }
    if (strcmp(m, "map") == 0) {
        if (is_ok_like && call->args.len > 0) {
            Value fn = eval_expr(in, env, (const SnExpr *)call->args.items[0]);
            if (fn.kind == V_LAMBDA) {
                Value mapped =
                    call_lambda_with_value(in, fn.as.lam, payload, call->span);
                SnList payload_list = {0};
                Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
                *slot = mapped;
                sn_list_push(in->arena, &payload_list, slot);
                *out = v_variant(in, vt->name, payload_list);
                return 1;
            }
        }
        *out = recv;
        return 1;
    }
    return 0;
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

    if (callee->kind == SN_EXPR_IDENT && callee->text) {
        if (strcmp(callee->text, "type") == 0) {
            /* type(T) expression returns type name as string */
            if (e->args.len > 0) {
                const SnExpr *arg0 = (const SnExpr *)e->args.items[0];
                if (arg0->text) {
                    return v_str(arg0->text);
                }
                Value av = eval_expr(in, env, arg0);
                if (av.kind == V_STRING) return av;
                return v_str(to_string(in, av, e->span));
            }
            return v_str("type");
        }
        Value *local = env_lookup(env, callee->text);
        if (local && local->kind == V_LAMBDA) {
            return call_lambda(in, local->as.lam, (SnList *)&e->args, env,
                               e->span);
        }
        const SnDecl *fn = find_top(in, callee->text, SN_DECL_FUNC);
        if (fn) {
            return call_function(in, fn, (SnList *)&e->args, env, NULL, e->span);
        }
        if (strcmp(callee->text, "Array") == 0) {
            Value v;
            v.kind = V_ARRAY;
            v.as.arr = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
            return v;
        }
        const SnDecl *cls = find_type(in, callee->text);
        if (cls) {
            Value v;
            v.kind = V_OBJECT;
            v.as.o = instantiate(in, cls, (SnList *)&e->args, env, e->span);
            return v;
        }
        if (is_variant_constructor(in, callee->text)) {
            return make_variant_from_args(in, env, callee->text, &e->args);
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown function `%s`",
                 callee->text);
        return v_unit();
    }

    /* Method call on a receiver. */
    if (callee->kind == SN_EXPR_MEMBER && callee->lhs) {
        /* Static method: `Foo.bar()` where Foo is a declared type, not a
         * local binding. This must be checked before evaluating `callee->lhs`
         * as a value expression below: a bare type name is not itself an
         * executable value, so eval_expr would raise "undefined name" for
         * any static call before this function ever got a chance to
         * recognize it as one. */
        if (callee->lhs->kind == SN_EXPR_IDENT && callee->lhs->text &&
            !env_lookup(env, callee->lhs->text)) {
            if (strcmp(callee->lhs->text, "Array") == 0) {
                Value v;
                v.kind = V_ARRAY;
                v.as.arr = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
                return v;
            }
            const SnDecl *cls = find_type(in, callee->lhs->text);
            if (cls) {
                const SnDecl *m = find_member_inherited(in, cls, callee->text);
                if (m) {
                    if (strcmp(m->name, "new") == 0) {
                        /* Canonical constructor: public static method new(...) { this.x = x } */
                        Object *obj = instantiate(in, cls, NULL, env, e->span);
                        Value res = call_method(in, obj, m, (SnList *)&e->args, env, e->span);
                        if (res.kind == V_OBJECT) {
                            return res;
                        }
                        Value out_obj;
                        out_obj.kind = V_OBJECT;
                        out_obj.as.o = obj;
                        return out_obj;
                    }
                    return call_function(in, m, (SnList *)&e->args, env, NULL,
                                         e->span);
                }
                if (strcmp(callee->text, "new") == 0) {
                    Value v;
                    v.kind = V_OBJECT;
                    v.as.o = instantiate(in, cls, (SnList *)&e->args, env, e->span);
                    return v;
                }
                rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                         "unknown method `%s`",
                         callee->text ? callee->text : "?");
                return v_unit();
            }
        }



        Value recv = eval_expr(in, env, callee->lhs);
        if (recv.kind == V_STRING && callee->text &&
            try_string_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_ARRAY && callee->text &&
            try_array_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_VARIANT && callee->text &&
            try_variant_method(in, env, recv, e, &out)) {
            return out;
        }
        if (recv.kind == V_OBJECT) {
            const SnDecl *m = find_member_inherited(in, recv.as.o->cls, callee->text);
            if (m) {
                return call_method(in, recv.as.o, m, (SnList *)&e->args, env,
                                   e->span);
            }
            /* A field that holds a lambda value is callable through the
             * member: `handler.onHit(entry)`. */
            Value *f = object_field(recv.as.o, callee->text);
            if (f && f->kind == V_LAMBDA) {
                return call_lambda(in, f->as.lam, (SnList *)&e->args, env,
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
        if (e->lhs->lhs && e->lhs->lhs->kind == SN_EXPR_IDENT && e->lhs->lhs->text &&
            !env_lookup(env, e->lhs->lhs->text)) {
            const SnDecl *cls = find_type(in, e->lhs->lhs->text);
            if (cls) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", e->lhs->lhs->text, e->lhs->text);
                slot = env_lookup(in->globals, key);
                if (!slot) {
                    slot = env_define(in, in->globals, key, val);
                }
            }
        }
        if (!slot) {
            Value recv = eval_expr(in, env, e->lhs->lhs);
            if (recv.kind == V_OBJECT) {
                slot = object_field(recv.as.o, e->lhs->text);
                if (!slot && recv.as.o) {
                    Value *new_slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
                    *new_slot = v_unit();
                    sn_list_push(in->arena, &recv.as.o->names, (void *)e->lhs->text);
                    sn_list_push(in->arena, &recv.as.o->slots, new_slot);
                    slot = new_slot;
                }
            }
        }
    } else if (e->lhs->kind == SN_EXPR_INDEX) {
        Value recv = eval_expr(in, env, e->lhs->lhs);
        if (recv.kind == V_ARRAY) {
            long long idx = as_int(in, eval_expr(in, env, e->lhs->rhs), e->span);
            if (idx >= 0 && (size_t)idx < recv.as.arr->items.len) {
                slot = (Value *)recv.as.arr->items.items[idx];
            }
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
        /* Payload-less variants read as bare identifiers: `None`, or a
         * user enum's empty variant. */
        if (e->text && is_variant_constructor(in, e->text)) {
            SnList empty = {0};
            return v_variant(in, e->text, empty);
        }
        rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "undefined name `%s`",
                 e->text);
        return v_unit();
    }
    case SN_EXPR_MEMBER: {
        /* Enum variant access: `State.Done` where `State` is a declared enum
         * type, not a local binding — mirrors the static-call check in
         * eval_call() for the analogous `Foo.bar()` case. Must run before
         * eval_expr(e->lhs) below: a bare type name is not itself an
         * executable value. */
        if (e->lhs->kind == SN_EXPR_IDENT && e->lhs->text && e->text &&
            !env_lookup(env, e->lhs->text)) {
            const SnDecl *en = find_top(in, e->lhs->text, SN_DECL_ENUM);
            if (en) {
                for (size_t i = 0; i < en->variants.len; i++) {
                    const SnDecl *v = (const SnDecl *)en->variants.items[i];
                    if (v->name && strcmp(v->name, e->text) == 0) {
                        SnList empty = {0};
                        return v_variant(in, v->name, empty);
                    }
                }
            }
            const SnDecl *cls = find_type(in, e->lhs->text);
            if (cls) {
                char key[512];
                snprintf(key, sizeof(key), "%s.%s", e->lhs->text, e->text);
                Value *slot = env_lookup(in->globals, key);
                if (slot) {
                    return *slot;
                }
                const SnDecl *f = find_member(cls, e->text);
                if (f) {
                    Value v = f->init ? eval_expr(in, env, f->init) : default_for(f->type);
                    env_define(in, in->globals, key, v);
                    return v;
                }
            }
            if (en) {
                rt_error(in, SNOVA_UNDEFINED_NAME, e->span,
                         "enum `%s` has no variant `%s`", e->lhs->text,
                         e->text);
                return v_unit();
            }
        }
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
    case SN_EXPR_LAMBDA: {
        LambdaVal *lam = (LambdaVal *)sn_arena_calloc(in->arena,
                                                      sizeof(LambdaVal));
        lam->expr = e;
        lam->env = env;
        Value v;
        v.kind = V_LAMBDA;
        v.as.lam = lam;
        return v;
    }
    case SN_EXPR_AWAIT:
        /* The smoke-path interpreter is synchronous: `await e` is `e`. */
        return eval_expr(in, env, e->lhs);
    case SN_EXPR_STRUCT_LIT: {
        if (!e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->lhs->text) {
            rt_error(in, SNOVA_UNSUPPORTED, e->span,
                     "struct literal target is not a plain type name");
            return v_unit();
        }
        const SnDecl *cls = find_type(in, e->lhs->text);
        if (!cls) {
            rt_error(in, SNOVA_UNDEFINED_NAME, e->span, "unknown type `%s`",
                     e->lhs->text);
            return v_unit();
        }
        Object *o = (Object *)sn_arena_calloc(in->arena, sizeof(Object));
        o->cls = cls;
        for (size_t i = 0; i < cls->members.len; i++) {
            const SnDecl *m = (const SnDecl *)cls->members.items[i];
            if (m->kind != SN_DECL_FIELD) {
                continue;
            }
            Value v = m->init ? eval_expr(in, env, m->init)
                              : default_for(m->type);
            for (size_t j = 0; j < e->field_names.len; j++) {
                const char *fname = (const char *)e->field_names.items[j];
                if (m->name && strcmp(fname, m->name) == 0) {
                    v = eval_expr(in, env, (const SnExpr *)e->args.items[j]);
                    break;
                }
            }
            Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
            *slot = v;
            sn_list_push(in->arena, &o->names, (void *)m->name);
            sn_list_push(in->arena, &o->slots, slot);
        }
        Value v;
        v.kind = V_OBJECT;
        v.as.o = o;
        return v;
    }
    case SN_EXPR_MATCH: {
        Value subject = eval_expr(in, env, e->lhs);
        if (in->failed) {
            return v_unit();
        }
        Env *arm_env = NULL;
        const SnMatchArm *arm =
            match_select_arm(in, env, &e->arms, subject, &arm_env);
        if (!arm) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "no match arm matched value `%s`",
                     to_string(in, subject, e->span));
            return v_unit();
        }
        if (arm->value) {
            return eval_expr(in, arm_env, arm->value);
        }
        if (arm->body) {
            /* Block-bodied arm in expression position: run it for effect; a
             * `return` inside is delivered as the surrounding function's
             * return value by the caller's exec loop. */
            Flow f = exec_stmt(in, arm_env, arm->body);
            (void)f;
        }
        return v_unit();
    }
    case SN_EXPR_INDEX: {
        Value base = eval_expr(in, env, e->lhs);
        if (base.kind != V_ARRAY) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span, "value is not indexable");
            return v_unit();
        }
        long long idx = as_int(in, eval_expr(in, env, e->rhs), e->span);
        if (idx < 0 || (size_t)idx >= base.as.arr->items.len) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "array index %lld out of bounds (len %zu)", idx,
                     base.as.arr->items.len);
            return v_unit();
        }
        return *(const Value *)base.as.arr->items.items[idx];
    }
    case SN_EXPR_ARRAY: {
        ArrayVal *arr = (ArrayVal *)sn_arena_calloc(in->arena, sizeof(ArrayVal));
        for (size_t i = 0; i < e->args.len; i++) {
            Value v = eval_expr(in, env, (const SnExpr *)e->args.items[i]);
            Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
            *slot = v;
            sn_list_push(in->arena, &arr->items, slot);
        }
        Value v;
        v.kind = V_ARRAY;
        v.as.arr = arr;
        return v;
    }
    default:
        rt_error(in, SNOVA_UNSUPPORTED, e->span,
                 "this expression form is not executable yet");
        return v_unit();
    }
}
