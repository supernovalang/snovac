/* eval.c — interpreter entry point, environments, object model and calls.
 * Expressions, statements and string decoding live in their own files; see
 * eval_internal.h for the split. */
#include "eval_internal.h"

void rt_error(Interp *in, int code, SnSpan span, const char *fmt, ...) {
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

Env *env_new(Interp *in, Env *parent) {
    Env *e = (Env *)sn_arena_calloc(in->arena, sizeof(Env));
    e->parent = parent;
    return e;
}

Value *env_lookup(Env *e, const char *name) {
    for (; e; e = e->parent) {
        for (size_t i = 0; i < e->names.len; i++) {
            if (strcmp((const char *)e->names.items[i], name) == 0) {
                return (Value *)e->slots.items[i];
            }
        }
    }
    return NULL;
}

Value *env_define(Interp *in, Env *e, const char *name, Value v) {
    Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
    *slot = v;
    sn_list_push(in->arena, &e->names, (void *)name);
    sn_list_push(in->arena, &e->slots, slot);
    return slot;
}

/* ── strings ──────────────────────────────────────────────────────────────── */

char *arena_sprintf(Interp *in, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    return sn_arena_strndup(in->arena, buf,
                            (size_t)n < sizeof(buf) ? (size_t)n
                                                    : sizeof(buf) - 1);
}

char *str_concat(Interp *in, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *out = (char *)sn_arena_alloc(in->arena, la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

const char *to_string(Interp *in, Value v, SnSpan span) {
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
        return arena_sprintf(in, "<%s>",
                             v.as.o->cls->name ? v.as.o->cls->name : "object");
    }
    }
    return "?";
}

/* ── declaration lookup ───────────────────────────────────────────────────── */

const SnDecl *find_member(const SnDecl *cls, const char *name) {
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

const SnDecl *find_top(const Interp *in, const char *name, SnDeclKind k) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if (d->kind == k && d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

const SnDecl *find_type(const Interp *in, const char *name) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if ((d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT) &&
            d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

/* ── objects ──────────────────────────────────────────────────────────────── */

Value default_for(const SnType *t) {
    if (t && t->name) {
        if (strcmp(t->name, "int") == 0 || strcmp(t->name, "long") == 0) return v_int(0);
        if (strcmp(t->name, "double") == 0 || strcmp(t->name, "decimal") == 0) return v_double(0);
        if (strcmp(t->name, "bool") == 0) return v_bool(0);
        if (strcmp(t->name, "string") == 0) return v_str("");
    }
    return v_unit();
}

Object *instantiate(Interp *in, const SnDecl *cls, SnList *args, Env *env,
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

Value *object_field(Object *o, const char *name) {
    for (size_t i = 0; i < o->names.len; i++) {
        if (strcmp((const char *)o->names.items[i], name) == 0) {
            return (Value *)o->slots.items[i];
        }
    }
    return NULL;
}

/* ── calls ────────────────────────────────────────────────────────────────── */

Value call_function(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
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

Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                  Env *caller, SnSpan span) {
    return call_function(in, m, args, caller, self, span);
}

/* ── entry point ──────────────────────────────────────────────────────────── */

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
