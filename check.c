#include "check.h"

#include <string.h>

#include "builtins.h"

void sn_checker_init(SnChecker *c, SnArena *a, SnInternTable *it, SnDiagSink *diag,
                      SnResolver *resolver, SnTypeTable *types) {
    c->arena = a;
    c->intern = it;
    c->diag = diag;
    c->resolver = resolver;
    c->types = types;
    c->current_package = NULL;
    c->current_imports = NULL;
    c->enclosing_type = NULL;
    c->current_return_type = NULL;
    c->type_params = NULL;
    c->in_constructor = 0;
    c->in_async_body = 0;
    c->in_pulsar_launch = 0;
    c->loop_depth = 0;
}

static int is_numeric(const SnTypeRep *t) {
    return t->tag == SN_T_INT || t->tag == SN_T_LONG || t->tag == SN_T_DOUBLE ||
           t->tag == SN_T_DECIMAL || t->tag == SN_T_FLOAT || t->tag == SN_T_BYTE;
}

static int is_error(const SnTypeRep *t) { return t->tag == SN_T_ERROR; }

/* An UNSUFFIXED numeric literal carries no committed type until context gives
 * it one: `3.0` is an equally valid `decimal` (tests/compile-pass/structs.snova
 * writes `Point(3.0, 4.0)` for two `decimal` fields), `float`
 * (tests/compile-pass/data_class.snova) or `double`.
 *
 * This is NOT the implicit numeric promotion ratified as forbidden (plan.md §7
 * item 1, measured 2026-07-25): that rule is about converting an
 * already-typed VALUE, and it still holds — `long + int` is still an error.
 * A literal has no value-level type to convert from. The lexer already draws
 * the line for us: `1`/`3.0` come through as SN_TOK_INT/SN_TOK_DOUBLE, while
 * the suffixed `42L`/`1.5d` come through as SN_TOK_LONG/SN_TOK_DECIMAL and are
 * committed, so they keep following the exact-match rule.
 *
 * Adopting rewrites the literal's own resolved_type, so the AST records the
 * type the literal actually has rather than the lexer's default. */
static SnTypeRep *adopt_literal_type(SnChecker *c, SnExpr *e, SnTypeRep *actual,
                                     SnTypeRep *expected) {
    if (!e || !expected || !actual || is_error(expected) || expected == actual) {
        return actual;
    }
    if (e->kind == SN_EXPR_UNARY && (e->op == SN_TOK_MINUS || e->op == SN_TOK_PLUS) &&
        e->lhs) {
        /* `-1` is still a literal; the sign doesn't commit it to a type. */
        SnTypeRep *inner = adopt_literal_type(c, e->lhs, actual, expected);
        if (inner != actual) {
            e->resolved_type = inner;
        }
        return inner;
    }
    int fits = 0;
    if (e->kind == SN_EXPR_INT) {
        fits = expected->tag == SN_T_INT || expected->tag == SN_T_LONG ||
               expected->tag == SN_T_BYTE;
    } else if (e->kind == SN_EXPR_DOUBLE) {
        fits = expected->tag == SN_T_DOUBLE || expected->tag == SN_T_FLOAT ||
               expected->tag == SN_T_DECIMAL;
    }
    if (!fits) {
        return actual;
    }
    e->resolved_type = expected;
    return expected;
}

/* `sub` is `super`, or inherits from it. Follows EVERY supertype listed after
 * `:`, not just the first: the parser deliberately does not tell a base class
 * from an implemented interface (ast.h, `supertypes`), so both count for
 * assignability. Supertype names are looked up in the current package's scope,
 * the same limitation lookup_member_with_inheritance already carries. */
static int decl_derives_from(SnChecker *c, const SnDecl *sub, const SnDecl *super,
                             int depth) {
    if (!sub || !super || depth > 8) {
        return 0;
    }
    if (sub == super) {
        return 1;
    }
    SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, c->current_package);
    for (size_t i = 0; i < sub->supertypes.len; i++) {
        const SnType *st = SN_LIST_AT(sub->supertypes, SnType, i);
        if (st->kind != SN_TYPE_NAME) {
            continue;
        }
        const char *sname = sn_intern_cstr(c->intern, st->name);
        SnSymbol *ssym = pkg_scope ? sn_scope_lookup_local(pkg_scope, sname) : NULL;
        if (ssym && ssym->kind == SN_SYM_TYPE &&
            decl_derives_from(c, ssym->decl, super, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

/* True when a value of type `value` cannot stand where `expected` is wanted.
 *
 * The order is (value, expected) at every call site and matters: assignability
 * is directional. A `Dog` may initialize an `Animal` binding
 * (tests/compile-pass/inheritance_polymorphism.snova) — the reverse may not.
 *
 * SN_T_ERROR means a failure was already reported upstream, and SN_T_ANY is
 * the corpus's escape hatch (`func printline(value: any)`); neither should
 * produce a second, derived diagnostic. */
static int types_clash(SnChecker *c, const SnTypeRep *value, const SnTypeRep *expected) {
    if (!value || !expected || is_error(value) || is_error(expected)) {
        return 0;
    }
    if (sn_type_is_any(value) || sn_type_is_any(expected)) {
        return 0;
    }
    if (value == expected) {
        return 0;
    }
    if (expected->tag == SN_T_NAMED && expected->decl &&
        strcmp(expected->decl->name, "Option") == 0 && expected->nargs == 1) {
        if (!types_clash(c, value, expected->args[0])) {
            return 0; /* Subsumption: T <: T? */
        }
    }
    if (value->tag == SN_T_NAMED && expected->tag == SN_T_NAMED && value->decl &&
        expected->decl &&
        decl_derives_from(c, value->decl->decl, expected->decl->decl, 0)) {
        return 0;
    }
    return 1;
}

/* ── AST type -> resolved type ────────────────────────────────────────────── */

static SnTypeRep *sn_check_resolve_type_base(SnChecker *c, const SnType *t) {
    if (!t) {
        return sn_type_error(c->types);
    }
    switch (t->kind) {
    case SN_TYPE_NAME: {
        /* A type parameter shadows everything — `T` inside `func f<T>()` is
         * that parameter, never some package's type named T. Checked before
         * sn_resolve_type_name so it never gets a chance to report SNOVA0027. */
        if (c->type_params) {
            SnSymbol *tp =
                sn_scope_lookup_local(c->type_params, sn_intern_cstr(c->intern, t->name));
            if (tp) {
                return sn_type_typevar(c->types, tp);
            }
        }
        /* Intrinsic generic constructors — `Array<T>`, `Partial<T>`. No
         * .snova file declares them, so they short-circuit before any scope
         * lookup, exactly as the primitives do inside sn_resolve_type_name.
         * Their arguments are resolved first because the constructed type
         * needs them; a non-intrinsic name still resolves its own name first,
         * so an unknown type keeps reporting one diagnostic and not also one
         * per unresolved argument. */
        if (sn_builtin_is_generic_name(c->intern, sn_intern_cstr(c->intern, t->name))) {
            SnTypeRep **iargs = NULL;
            if (t->args.len) {
                iargs = (SnTypeRep **)sn_arena_alloc(c->arena,
                                                     t->args.len * sizeof(SnTypeRep *));
                for (size_t i = 0; i < t->args.len; i++) {
                    iargs[i] = sn_check_resolve_type(c, SN_LIST_AT(t->args, SnType, i));
                }
            }
            return sn_builtin_generic(c->types, c->intern,
                                      sn_intern_cstr(c->intern, t->name), iargs,
                                      (uint32_t)t->args.len);
        }

        SnTypeRep *base = sn_resolve_type_name(c->resolver, c->current_package,
                                               c->current_imports, t->name, t->span);
        if (!base) {
            return sn_type_error(c->types);
        }
        if (t->args.len == 0 || !base->decl) {
            /* No generic args, or a primitive (which never legitimately
             * takes args in this corpus) — degrade gracefully rather than
             * fabricate a shape types.c can't represent. */
            return base;
        }
        SnTypeRep **args =
            (SnTypeRep **)sn_arena_alloc(c->arena, t->args.len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < t->args.len; i++) {
            SnType *arg_t = SN_LIST_AT(t->args, SnType, i);
            args[i] = sn_check_resolve_type(c, arg_t);
        }
        return sn_type_named(c->types, base->decl, args, (uint32_t)t->args.len);
    }
    case SN_TYPE_FUNC: {
        SnTypeRep **params = NULL;
        if (t->params.len) {
            params =
                (SnTypeRep **)sn_arena_alloc(c->arena, t->params.len * sizeof(SnTypeRep *));
            for (size_t i = 0; i < t->params.len; i++) {
                SnType *pt = SN_LIST_AT(t->params, SnType, i);
                params[i] = sn_check_resolve_type(c, pt);
            }
        }
        SnTypeRep *ret = sn_check_resolve_type(c, t->ret);
        return sn_type_func(c->types, params, (uint32_t)t->params.len, ret);
    }
    case SN_TYPE_TUPLE:
        /* types.c's SnTypeTag (plan.md §5) has no tuple tag. Documented gap
         * — degrade rather than mis-tag a tuple as something it isn't. */
        return sn_type_error(c->types);
    }
    return sn_type_error(c->types);
}

SnTypeRep *sn_check_resolve_type(SnChecker *c, const SnType *t) {
    SnTypeRep *res = sn_check_resolve_type_base(c, t);
    if (t && t->is_optional && res != sn_type_error(c->types)) {
        res = sn_builtin_generic(c->types, c->intern,
                                  sn_intern_cstr(c->intern, "Option"), &res, 1);
    }
    return res;
}

/* ── symbol -> type, and the linear-scan helpers that back it ────────────── */

static SnSymbol *find_type_symbol_for_decl(SnResolver *r, const SnDecl *type_decl) {
    for (SnPackageScopeEntry *e = r->packages; e; e = e->next) {
        for (size_t bi = 0; bi < e->scope->nbuckets; bi++) {
            for (SnSymbol *s = e->scope->buckets[bi]; s; s = s->next) {
                if (s->kind == SN_SYM_TYPE && s->decl == type_decl) {
                    return s;
                }
            }
        }
    }
    return NULL;
}

static SnSymbol *find_variant_owner_symbol(SnResolver *r, const SnSymbol *variant_sym) {
    const SnDecl *owner_decl = NULL;
    for (SnTypeScopeEntry *e = r->type_scopes; e; e = e->next) {
        SnSymbol *found = sn_scope_lookup_local(e->member_scope, variant_sym->name);
        if (found == variant_sym) {
            owner_decl = e->type_decl;
            break;
        }
    }
    return owner_decl ? find_type_symbol_for_decl(r, owner_decl) : NULL;
}

static SnTypeRep *func_like_type(SnChecker *c, const SnDecl *decl) {
    SnTypeRep **params = NULL;
    if (decl->params.len) {
        params =
            (SnTypeRep **)sn_arena_alloc(c->arena, decl->params.len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < decl->params.len; i++) {
            SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
            params[i] = sn_check_resolve_type(c, p->type);
        }
    }
    SnTypeRep *ret = sn_check_resolve_type(c, decl->ret);
    return sn_type_func(c->types, params, (uint32_t)decl->params.len, ret);
}

static SnTypeRep *symbol_type_in_scope(SnChecker *c, SnSymbol *sym);
static void bind_pattern_names(SnChecker *c, SnScope *scope, const SnPattern *pat);
static void check_match_exhaustiveness(SnChecker *c, const SnTypeRep *target_ty,
                                       const SnList *arms, SnSpan span);

/* True when `sym` was declared in a file other than the one being checked.
 * Resolving such a symbol's declared types re-runs name resolution in the
 * CURRENT context, where the other file's imports and type parameters do not
 * exist — `T` in builtin/Errors.snova's `func try<T, E>` is not in scope at a
 * call site in a fixture. Whatever it reports there would be both wrong and
 * attributed to the wrong file, and the real problem (if any) is reported
 * where that declaration's own body is checked. */
static int symbol_is_foreign(const SnChecker *c, const SnSymbol *sym) {
    return sym && sym->origin && sym->origin->path != c->diag->file.path;
}

/* Resolves a type as written in `sym`'s declaration. Silent when `sym` is
 * foreign: the resolution runs in the current file's scope, which is the
 * wrong one for it, so anything it would report is noise here. */
static SnTypeRep *resolve_declared_type(SnChecker *c, const SnSymbol *sym,
                                        const SnType *t) {
    int foreign = symbol_is_foreign(c, sym);
    c->diag->quiet += foreign;
    SnTypeRep *ty = sn_check_resolve_type(c, t);
    c->diag->quiet -= foreign;
    return ty;
}

static SnTypeRep *symbol_type(SnChecker *c, SnSymbol *sym) {
    if (!sym) {
        return sn_type_error(c->types);
    }
    int foreign = symbol_is_foreign(c, sym);
    c->diag->quiet += foreign;
    SnTypeRep *ty = symbol_type_in_scope(c, sym);
    c->diag->quiet -= foreign;
    return ty;
}

static SnTypeRep *symbol_type_in_scope(SnChecker *c, SnSymbol *sym) {
    switch (sym->kind) {
    case SN_SYM_LOCAL:
    case SN_SYM_PARAM:
        return sym->value_type ? sym->value_type : sn_type_error(c->types);
    case SN_SYM_FIELD:
    case SN_SYM_CONST:
        return sn_check_resolve_type(c, sym->decl->type);
    case SN_SYM_FUNC:
    case SN_SYM_METHOD:
        return func_like_type(c, sym->decl);
    case SN_SYM_TYPE:
        return sn_type_named(c->types, sym, NULL, 0);
    case SN_SYM_VARIANT:
        /* A bare reference to a variant name without calling it (no corpus
         * evidence this happens — variants are always constructed,
         * `Some(x)`/`None`) has no clean type here; the CALL path handles
         * construction separately via find_variant_owner_symbol. */
        return sn_type_error(c->types);
    case SN_SYM_PACKAGE:
        return sn_type_error(c->types); /* not a value */
    }
    return sn_type_error(c->types);
}

static int is_mutable_symbol(const SnChecker *c, const SnSymbol *sym) {
    if (!sym) {
        return 1; /* unresolved: already diagnosed elsewhere, don't cascade */
    }
    switch (sym->kind) {
    case SN_SYM_LOCAL:
        return sym->is_mutable;
    case SN_SYM_FIELD:
        /* A `let` field is immutable everywhere EXCEPT the constructor, which
         * is where it gets its one value — `this.description = description` in
         * tests/compile-pass/p1_syntax_additions.snova. Assigning it twice is
         * a definite-assignment question, and flow analysis is P4. */
        return (sym->decl && sym->decl->is_mutable) || c->in_constructor;
    default:
        return 0;
    }
}

/* ── member lookup with single-inheritance walk (mirrors sn_resolve_ident's,
 * kept local since resolve.c doesn't expose its internal walk as a
 * standalone function) ──────────────────────────────────────────────────── */

static SnSymbol *lookup_member_with_inheritance(SnChecker *c, const SnDecl *type_decl,
                                                const char *name) {
    const SnDecl *cur = type_decl;
    int depth = 0;
    while (cur && depth < 64) {
        SnScope *ms = sn_resolver_type_scope(c->resolver, cur);
        if (ms) {
            SnSymbol *sym = sn_scope_lookup_local(ms, name);
            if (sym) {
                return sym;
            }
        }
        if (cur->supertypes.len == 0) {
            break;
        }
        SnType *super_ty = SN_LIST_AT(cur->supertypes, SnType, 0);
        if (super_ty->kind != SN_TYPE_NAME) {
            break;
        }
        const char *super_name = sn_intern_cstr(c->intern, super_ty->name);
        SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, c->current_package);
        SnSymbol *super_sym =
            pkg_scope ? sn_scope_lookup_local(pkg_scope, super_name) : NULL;
        cur = (super_sym && super_sym->kind == SN_SYM_TYPE) ? super_sym->decl : NULL;
        depth++;
    }
    return NULL;
}

/* Resolves what an IDENT or MEMBER expression refers to, checking/caching
 * it (and, for MEMBER, its receiver chain) along the way. Shared by
 * sn_check_expr's own IDENT/MEMBER cases and by CALL, which needs the same
 * resolution for its callee without checking it twice. */
static const char *resolve_package_prefix(SnChecker *c, SnScope *local, SnExpr *e) {
    if (e->kind == SN_EXPR_IDENT) {
        const char *head_i = sn_intern_cstr(c->intern, e->text);
        if (local && sn_scope_lookup(local, head_i)) return NULL;
        if (c->enclosing_type) {
            SnScope *ms = sn_resolver_type_scope(c->resolver, c->enclosing_type);
            if (ms && sn_scope_lookup_local(ms, head_i)) return NULL;
        }
        SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, c->current_package);
        if (pkg_scope && sn_scope_lookup_local(pkg_scope, head_i)) return NULL;

        if (c->current_imports) {
            for (size_t i = 0; i < c->current_imports->len; i++) {
                const char *imp_pkg = SN_LIST_AT(*c->current_imports, const char, i);
                const char *last = strrchr(imp_pkg, '.');
                const char *stem = last ? last + 1 : imp_pkg;
                if (strcmp(stem, e->text) == 0) {
                    return imp_pkg;
                }
            }
        }
        if (sn_pkggraph_find(c->resolver->graph, head_i)) return head_i;
        return NULL;
    }
    if (e->kind == SN_EXPR_MEMBER) {
        const char *parent_pkg = resolve_package_prefix(c, local, e->lhs);
        if (parent_pkg) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s.%s", parent_pkg, e->text);
            const char *candidate = sn_intern_cstr(c->intern, buf);
            if (sn_pkggraph_find(c->resolver->graph, candidate)) return candidate;
        }
    }
    return NULL;
}

/* `Array<Post>` / `Partial<User>` written in EXPRESSION position — a
 * reference to an intrinsic TYPE, not a value. The parser leaves these as an
 * IDENT carrying `type_args` (parse_expr.c, "the generic arguments belong to
 * the type"), so there is no symbol to resolve and sn_resolve_ident would
 * report SNOVA0023 for a name that is perfectly real. Returns the type, or
 * NULL when `e` is not one of these. */
static SnTypeRep *intrinsic_generic_ref(SnChecker *c, const char *name,
                                        const SnList *type_args) {
    if (!name) {
        return NULL;
    }
    const char *iname = sn_intern_cstr(c->intern, name);
    if (!sn_builtin_is_generic_name(c->intern, iname)) {
        return NULL;
    }
    SnTypeRep **args = NULL;
    if (type_args->len) {
        args = (SnTypeRep **)sn_arena_alloc(c->arena,
                                            type_args->len * sizeof(SnTypeRep *));
        for (size_t i = 0; i < type_args->len; i++) {
            args[i] = sn_check_resolve_type(c, SN_LIST_AT(*type_args, SnType, i));
        }
    }
    return sn_builtin_generic(c->types, c->intern, iname, args, (uint32_t)type_args->len);
}

static SnTypeRep *intrinsic_type_ref(SnChecker *c, SnExpr *e) {
    if (e->kind != SN_EXPR_IDENT || !e->text) {
        return NULL;
    }
    return intrinsic_generic_ref(c, e->text, &e->type_args);
}

static SnSymbol *resolve_value_symbol(SnChecker *c, SnScope *local, SnExpr *e) {
    if (e->kind == SN_EXPR_IDENT) {
        SnSymbol *sym = sn_resolve_ident(c->resolver, c->current_package, local,
                                         c->enclosing_type, c->current_imports, e->text,
                                         e->span);
        e->resolved_type = symbol_type(c, sym);
        return sym;
    }
    if (e->kind == SN_EXPR_MEMBER) {
        /* `Array<Post>.new()` — receiver is an intrinsic type, not a value.
         * Checked before the package-prefix walk so the receiver is never
         * evaluated as an expression (which would report SNOVA0023 for it). */
        SnTypeRep *recv_ty = intrinsic_type_ref(c, e->lhs);
        if (recv_ty) {
            e->lhs->resolved_type = recv_ty;
            const char *sname = sn_intern_cstr(c->intern, e->text);
            SnTypeRep *sm = sn_builtin_static_member(c->types, c->intern, recv_ty, sname);
            if (!sm) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, e->span,
                            "type has no member `%s`", e->text);
                sm = sn_type_error(c->types);
            }
            e->resolved_type = sm;
            return NULL; /* intrinsic: no symbol — see the CALL case */
        }

        const char *pkg_prefix = resolve_package_prefix(c, local, e->lhs);
        if (pkg_prefix) {
            SnScope *pkg_scope = sn_resolver_package_scope(c->resolver, pkg_prefix);
            if (pkg_scope) {
                const char *mname = sn_intern_cstr(c->intern, e->text);
                SnSymbol *msym = sn_scope_lookup_local(pkg_scope, mname);
                if (msym) {
                    e->lhs->resolved_type = sn_type_error(c->types); /* package prefix is not a value */
                    e->resolved_type = symbol_type(c, msym);
                    return msym;
                }
            }
        }

        SnTypeRep *base_ty = sn_check_expr(c, local, e->lhs);
        if (is_error(base_ty)) {
            e->resolved_type = sn_type_error(c->types);
            return NULL;
        }
        const char *mname = sn_intern_cstr(c->intern, e->text);
        int is_qdot = (e->op == SN_TOK_QDOT);
        int is_opt_base = (base_ty->tag == SN_T_NAMED && base_ty->decl &&
                           strcmp(base_ty->decl->name, "Option") == 0 && base_ty->nargs == 1);
        if (is_qdot && !is_opt_base) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_OPTIONAL_CHAINING_NON_OPTIONAL, e->span,
                         "redundant optional chaining `?.` applied to non-optional type");
        }
        SnTypeRep *target_ty = (is_qdot && is_opt_base) ? base_ty->args[0] : base_ty;

        if (target_ty->tag != SN_T_NAMED || !target_ty->decl) {
            /* Arrays, strings and scalars carry members that no .snova file
             * declares (`arr.len()`, `n.toString()`) — builtins.c owns them. */
            SnTypeRep *bm = sn_builtin_member(c->types, c->intern, target_ty, mname);
            if (bm) {
                if (is_qdot && is_opt_base && !is_error(bm)) {
                    bm = sn_builtin_generic(c->types, c->intern, sn_intern_cstr(c->intern, "Option"), &bm, 1);
                }
                e->resolved_type = bm;
                return NULL; /* intrinsic: no symbol — see the CALL case */
            }
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, e->span,
                        "type has no member `%s`", e->text);
            e->resolved_type = sn_type_error(c->types);
            return NULL;
        }
        SnSymbol *msym = lookup_member_with_inheritance(c, target_ty->decl->decl, mname);
        if (!msym) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER, e->span,
                        "Unknown member `%s` on `%s`", e->text, target_ty->decl->name);
            e->resolved_type = sn_type_error(c->types);
            return NULL;
        }
        if (msym->decl && msym->decl->vis == SN_VIS_PRIVATE &&
            c->enclosing_type != target_ty->decl->decl) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PRIVATE_ACCESS, e->span,
                        "`%s` is private to `%s`", e->text, target_ty->decl->name);
        }
        SnTypeRep *mty = symbol_type(c, msym);
        if (target_ty->nargs > 0 && target_ty->decl && target_ty->decl->decl &&
            target_ty->decl->decl->generics.len > 0) {
            uint32_t count = (uint32_t)target_ty->decl->decl->generics.len;
            if (count > target_ty->nargs) {
                count = target_ty->nargs;
            }
            const char **param_names = (const char **)sn_arena_alloc(c->arena, count * sizeof(const char *));
            for (uint32_t gi = 0; gi < count; gi++) {
                param_names[gi] = SN_LIST_AT(target_ty->decl->decl->generics, const char, gi);
            }
            mty = sn_type_subst_names(c->types, mty, param_names, target_ty->args, count);
        }
        if (is_qdot && is_opt_base && !is_error(mty)) {
            mty = sn_builtin_generic(c->types, c->intern, sn_intern_cstr(c->intern, "Option"), &mty, 1);
        }
        e->resolved_type = mty;
        return msym;
    }
    sn_check_expr(c, local, e);
    return NULL;
}

/* ── operators ────────────────────────────────────────────────────────────── */

static int is_comparison_op(SnTokKind op) {
    switch (op) {
    case SN_TOK_LT: case SN_TOK_GT: case SN_TOK_LE: case SN_TOK_GE:
    case SN_TOK_EQ: case SN_TOK_NE:
        return 1;
    default:
        return 0;
    }
}

static SnTypeRep *check_binary(SnChecker *c, SnExpr *e, SnTypeRep *lt, SnTypeRep *rt) {
    if (is_error(lt) || is_error(rt)) {
        return sn_type_error(c->types); /* plan.md §5.1: silent propagation */
    }
    if (sn_type_is_any(lt) || sn_type_is_any(rt)) {
        /* types.h: `any` never takes part in a mismatch, in either direction.
         * A comparison still yields bool; everything else stays `any` rather
         * than guessing which operand's type wins. */
        return is_comparison_op(e->op) ? sn_type_bool(c->types) : sn_type_any(c->types);
    }
    switch (e->op) {
    case SN_TOK_PLUS:
        if (lt->tag == SN_T_STRING && rt->tag == SN_T_STRING) {
            return sn_type_string(c->types);
        }
        /* fallthrough */
    case SN_TOK_MINUS:
    case SN_TOK_STAR:
    case SN_TOK_SLASH:
    case SN_TOK_PERCENT:
    case SN_TOK_AMP:
    case SN_TOK_PIPE:
    case SN_TOK_CARET:
    case SN_TOK_SHL:
    case SN_TOK_SHR:
        if (is_numeric(lt) && lt == rt) {
            return lt;
        }
        break;
    case SN_TOK_LT:
    case SN_TOK_GT:
    case SN_TOK_LE:
    case SN_TOK_GE:
        if (is_numeric(lt) && lt == rt) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_EQ:
    case SN_TOK_NE:
        if (lt == rt) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_ANDAND:
    case SN_TOK_OROR:
        if (lt->tag == SN_T_BOOL && rt->tag == SN_T_BOOL) {
            return sn_type_bool(c->types);
        }
        break;
    case SN_TOK_QQ: {
        int is_opt = (lt->tag == SN_T_NAMED && lt->decl &&
                      strcmp(lt->decl->name, "Option") == 0 && lt->nargs == 1);
        if (!is_opt) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_REDUNDANT_NULL_COALESCING, e->span,
                         "redundant null-coalescing operator `??` applied to non-optional type");
            return lt;
        }
        SnTypeRep *inner = lt->args[0];
        if (rt != inner) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_MISMATCHED_FALLBACK, e->span,
                         "null-coalescing fallback type mismatch: expected inner type of Option");
            return sn_type_error(c->types);
        }
        return inner;
    }
    default:
        break;
    }
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BINARY_TYPE_MISMATCH, e->span,
                "operator `%s` cannot be applied to these operand types "
                "(no implicit numeric promotion — operands must match exactly)",
                sn_tok_name(e->op));
    return sn_type_error(c->types);
}

static SnTypeRep *check_unary(SnChecker *c, SnExpr *e, SnTypeRep *operand) {
    if (is_error(operand)) {
        return sn_type_error(c->types);
    }
    if (sn_type_is_any(operand)) {
        return operand; /* types.h: `any` never takes part in a mismatch */
    }

    if (e->op == SN_TOK_QUESTION) {
        /* PROVISIONAL — see check.h file header. plan.md §7 item 6 was left
         * unratified by resolve.c; real corpus usage is essentially absent. */
        if (operand->tag == SN_T_NAMED && operand->decl && operand->nargs >= 1 &&
            (operand->decl->name == sn_intern_cstr(c->intern, "Option") ||
             operand->decl->name == sn_intern_cstr(c->intern, "Result"))) {
            return operand->args[0];
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                    "`?` requires an Option or Result operand");
        return sn_type_error(c->types);
    }

    switch (e->op) {
    case SN_TOK_BANG:
        if (operand->tag == SN_T_BOOL) {
            return operand;
        }
        break;
    case SN_TOK_MINUS:
    case SN_TOK_PLUS:
        if (is_numeric(operand)) {
            return operand;
        }
        break;
    case SN_TOK_TILDE:
        if (operand->tag == SN_T_INT || operand->tag == SN_T_LONG) {
            return operand;
        }
        break;
    default:
        break;
    }
    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                "invalid operand type for unary `%s`", sn_tok_name(e->op));
    return sn_type_error(c->types);
}

/* ── calls ────────────────────────────────────────────────────────────────── */

static void check_call_args(SnChecker *c, SnExpr *call_expr, const SnSymbol *callee,
                            const SnList *params, SnTypeRep **arg_tys, size_t nargs) {
    if (params->len != nargs) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %zu argument(s), found %zu", params->len, nargs);
        return; /* don't also report per-argument mismatches against a
                 * signature that's already known not to match */
    }
    for (size_t i = 0; i < nargs; i++) {
        SnParam *p = SN_LIST_AT(*params, SnParam, i);
        SnTypeRep *expected = resolve_declared_type(c, callee, p->type);
        arg_tys[i] = adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                        arg_tys[i], expected);
        if (types_clash(c, arg_tys[i], expected)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu (`%s`) has the wrong type", i + 1, p->name);
        }
    }
}

/* Calling a VALUE of function type: a lambda held in a local
 * (`let double = (x: int) -> x * 2; double(n)`), a function-typed parameter, a
 * function-typed field. The signature comes from the type, not from a
 * declaration, so there are no parameter names to quote. */
static void check_call_against_functype(SnChecker *c, SnExpr *call_expr,
                                        const SnTypeRep *ft, SnTypeRep **arg_tys,
                                        size_t nargs) {
    if (ft->nargs != nargs) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %u argument(s), found %zu", ft->nargs, nargs);
        return;
    }
    for (size_t i = 0; i < nargs; i++) {
        arg_tys[i] = adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, i),
                                        arg_tys[i], ft->args[i]);
        if (types_clash(c, arg_tys[i], ft->args[i])) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu has the wrong type", i + 1);
        }
    }
}

/* The named type a constructor call produces, with any explicit generic
 * arguments threaded through so `Deferred<int, string>` and
 * `Deferred<string, int>` stay distinct (types.c hash-consing). A call that
 * writes none gets the bare named type — inferring generic arguments from the
 * argument list is not modeled (check.h's generics gap). */
static SnTypeRep *named_with_type_args(SnChecker *c, SnSymbol *type_sym,
                                       const SnExpr *call) {
    if (call->type_args.len == 0) {
        return sn_type_named(c->types, type_sym, NULL, 0);
    }
    if (type_sym->decl && call->type_args.len != type_sym->decl->generics.len) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_GENERIC_ARITY_MISMATCH, call->span,
                    "`%s` expects %zu type argument(s), found %zu",
                    type_sym->decl->name ? type_sym->decl->name : "<type>",
                    type_sym->decl->generics.len, call->type_args.len);
    }
    SnTypeRep **args =
        (SnTypeRep **)sn_arena_alloc(c->arena, call->type_args.len * sizeof(SnTypeRep *));
    for (size_t i = 0; i < call->type_args.len; i++) {
        args[i] = sn_check_resolve_type(c, SN_LIST_AT(call->type_args, SnType, i));
    }
    return sn_type_named(c->types, type_sym, args, (uint32_t)call->type_args.len);
}

/* Construction: `Point(3.0, 4.0)`, `Counter(0)`, `HealthCheck()`.
 *
 * ast.h already records the shape ("Construction has no `new` keyword"), and
 * no fixture or builtin file declares a constructor member — the class/struct
 * FIELD list, in source order, IS the signature. Fields carrying an
 * initializer are optional, so an accepted call passes between "fields without
 * an initializer" and "all fields" arguments; argument types are only matched
 * positionally when every field was supplied, since a partial call gives no
 * way to tell which fields the arguments belong to.
 *
 * Enums and interfaces are not constructible this way and are left alone. */
static void check_constructor_call(SnChecker *c, SnExpr *call_expr, SnSymbol *type_sym,
                                   SnTypeRep **arg_tys, size_t nargs) {
    const SnDecl *d = type_sym->decl;
    if (!d || (d->kind != SN_DECL_CLASS && d->kind != SN_DECL_STRUCT)) {
        return;
    }

    if (d->is_abstract) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ABSTRACT_INSTANTIATION, call_expr->span,
                    "cannot instantiate abstract class `%s`",
                    d->name ? d->name : "<type>");
        return;
    }

    /* An explicitly written signature wins over the field list: the
     * `class Foo(a: int)` primary-constructor form (parse_decl.c keeps it in
     * `params`), then a `constructor(...)` member (parse_decl.c parses it as a
     * method of that fixed name — "P2 owns its special dispatch"). */
    if (d->params.len) {
        check_call_args(c, call_expr, type_sym, &d->params, arg_tys, nargs);
        return;
    }
    for (size_t i = 0; i < d->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
        if (m->kind == SN_DECL_METHOD && m->name &&
            sn_intern_cstr(c->intern, m->name) == sn_intern_cstr(c->intern, "constructor")) {
            check_call_args(c, call_expr, type_sym, &m->params, arg_tys, nargs);
            return;
        }
    }

    size_t total = 0, required = 0;
    for (size_t i = 0; i < d->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        total++;
        if (!m->init) {
            required++;
        }
    }
    if (nargs < required || nargs > total) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, call_expr->span,
                    "expected %zu argument(s), found %zu", required, nargs);
        return;
    }
    if (nargs != total) {
        return;
    }
    size_t fi = 0;
    for (size_t i = 0; i < d->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        SnTypeRep *expected = resolve_declared_type(c, type_sym, m->type);
        arg_tys[fi] = adopt_literal_type(c, SN_LIST_AT(call_expr->args, SnExpr, fi),
                                         arg_tys[fi], expected);
        if (types_clash(c, arg_tys[fi], expected)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, call_expr->span,
                        "argument %zu (`%s`) has the wrong type", fi + 1, m->name);
        }
        fi++;
    }
}

/* ── the big expression switch ───────────────────────────────────────────── */

SnTypeRep *sn_check_expr(SnChecker *c, SnScope *local, SnExpr *e) {
    SnTypeRep *result;

    /* A recovered parse leaves holes: `a +` produces an SN_EXPR_BINARY whose
     * rhs never materialised, and the parser has already reported why. Those
     * nodes still reach here whenever anything type-checks a unit that did
     * not parse cleanly, so every operand slot is nullable by contract, not
     * by accident. Answering `error` keeps the walk going without inventing a
     * second diagnostic for a hole the parser already owns. */
    if (!e) {
        return sn_type_error(c->types);
    }

    switch (e->kind) {
    case SN_EXPR_INT:
        result = sn_type_int(c->types);
        break;
    case SN_EXPR_LONG:
        result = sn_type_long(c->types);
        break;
    case SN_EXPR_DOUBLE:
        result = sn_type_double(c->types);
        break;
    case SN_EXPR_DECIMAL:
        result = sn_type_decimal(c->types);
        break;
    case SN_EXPR_STRING:
        /* Interpolation contents aren't statically typed — see check.h. */
        result = sn_type_string(c->types);
        break;
    case SN_EXPR_CHAR:
        result = sn_type_char(c->types);
        break;
    case SN_EXPR_BOOL:
        result = sn_type_bool(c->types);
        break;

    case SN_EXPR_THIS: {
        SnSymbol *self_sym =
            c->enclosing_type ? find_type_symbol_for_decl(c->resolver, c->enclosing_type)
                              : NULL;
        result = self_sym ? sn_type_named(c->types, self_sym, NULL, 0)
                          : sn_type_error(c->types);
        break;
    }

    case SN_EXPR_IDENT:
    case SN_EXPR_MEMBER:
        resolve_value_symbol(c, local, e);
        result = e->resolved_type;
        break;

    case SN_EXPR_CALL: {
        /* `Partial<User>(name: "Ada", age: 36)` — construction of an
         * intrinsic generic. No symbol exists for the callee; the type IS the
         * result. Resolved before resolve_value_symbol so the callee is never
         * looked up as a value.
         *
         * Unlike the `Array<Post>.new()` shape (where the postfix loop
         * repurposes the CALL node into an IDENT and its `type_args` land on
         * that same node — see intrinsic_type_ref's header comment), a DIRECT
         * call keeps `e->lhs` as the plain, unmodified callee ident and
         * attaches `<User>` to the CALL node `e` itself (parse_expr.c:
         * `e->type_args = args` in the postfix loop, before `parse_call_args`
         * fills `e->args`). Looking up type args on `e->lhs` here always saw
         * an empty list, so `Partial<T>`/`Array<T>` direct-call construction
         * silently typed as `any` (0 type args) instead of `T`. */
        SnTypeRep *intrinsic_ctor = e->lhs->kind == SN_EXPR_IDENT
            ? intrinsic_generic_ref(c, e->lhs->text, &e->type_args)
            : NULL;
        SnSymbol *callee_sym =
            intrinsic_ctor ? NULL : resolve_value_symbol(c, local, e->lhs);
        if (intrinsic_ctor) {
            e->lhs->resolved_type = intrinsic_ctor;
        }
        /* Consumed here, before argument-checking recurses into anything
         * that might itself be a `pulsar work()` launch — SNOVA124 exempts
         * only the CALL directly named by a `pulsar` statement's own expr,
         * never a call nested inside its arguments or a launch nested
         * elsewhere in the tree. */
        int is_pulsar_launch_call = c->in_pulsar_launch;
        c->in_pulsar_launch = 0;

        SnTypeRep **arg_tys = NULL;
        if (e->args.len) {
            arg_tys =
                (SnTypeRep **)sn_arena_alloc(c->arena, e->args.len * sizeof(SnTypeRep *));
        }
        for (size_t i = 0; i < e->args.len; i++) {
            SnExpr *arg = SN_LIST_AT(e->args, SnExpr, i);
            arg_tys[i] = sn_check_expr(c, local, arg);
        }

        if (intrinsic_ctor) {
            result = intrinsic_ctor; /* arguments not matched — see builtins.h */
            break;
        }
        if (!callee_sym) {
            /* resolve_value_symbol returns NULL both for a real failure (it
             * has already reported one, and left SN_T_ERROR behind) and for an
             * intrinsic member, which has no symbol but does leave a real
             * SN_T_FUNC type behind. The latter is callable; its argument list
             * is not checked because builtins.c models no parameters. */
            SnTypeRep *callee_ty = e->lhs->resolved_type;
            result = (callee_ty && callee_ty->tag == SN_T_FUNC) ? callee_ty->ret
                                                                : sn_type_error(c->types);
            break;
        }
        if (callee_sym->kind == SN_SYM_FUNC || callee_sym->kind == SN_SYM_METHOD) {
            if (callee_sym->decl->is_pulsar && c->in_async_body) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, e->span,
                             "cannot call pulsar function `%s` inside an async function",
                             callee_sym->name);
            } else if (callee_sym->decl->is_pulsar && !is_pulsar_launch_call) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_DIRECT_CALL, e->span,
                            "pulsar function `%s` cannot be called directly — use `pulsar %s(...)`",
                            callee_sym->name, callee_sym->name);
            }
            check_call_args(c, e, callee_sym, &callee_sym->decl->params, arg_tys, e->args.len);
            result = resolve_declared_type(c, callee_sym, callee_sym->decl->ret);
            break;
        }
        if (callee_sym->kind == SN_SYM_VARIANT) {
            check_call_args(c, e, callee_sym, &callee_sym->decl->params, arg_tys, e->args.len);
            SnSymbol *owner_sym = find_variant_owner_symbol(c->resolver, callee_sym);
            result = owner_sym ? sn_type_named(c->types, owner_sym, NULL, 0)
                               : sn_type_error(c->types);
            break;
        }
        if (callee_sym->kind == SN_SYM_TYPE) {
            check_constructor_call(c, e, callee_sym, arg_tys, e->args.len);
            result = named_with_type_args(c, callee_sym, e);
            break;
        }
        /* A value whose type is a function — a lambda in a local, a
         * function-typed parameter or field. */
        SnTypeRep *callee_ty = e->lhs->resolved_type;
        if (callee_ty && callee_ty->tag == SN_T_FUNC) {
            check_call_against_functype(c, e, callee_ty, arg_tys, e->args.len);
            result = callee_ty->ret;
            break;
        }
        if (callee_ty && (is_error(callee_ty) || sn_type_is_any(callee_ty))) {
            /* Already-failed or deliberately untyped callee: plan.md §5.1
             * silent propagation, no second diagnostic. */
            result = sn_type_error(c->types);
            break;
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NOT_CALLABLE, e->lhs->span,
                    "`%s` is not callable", e->lhs->text ? e->lhs->text : "<expr>");
        result = sn_type_error(c->types);
        break;
    }

    case SN_EXPR_INDEX: {
        SnTypeRep *base_ty = sn_check_expr(c, local, e->lhs);
        SnTypeRep *idx_ty = sn_check_expr(c, local, e->rhs);
        (void)idx_ty; /* not enforced to be `int` in this pass — documented gap */
        if (is_error(base_ty)) {
            result = sn_type_error(c->types);
            break;
        }
        SnTypeRep *elem = sn_builtin_index_result(c->types, c->intern, base_ty);
        if (elem) {
            result = elem;
            break;
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                    "type is not indexable");
        result = sn_type_error(c->types);
        break;
    }

    case SN_EXPR_UNARY: {
        SnTypeRep *operand = sn_check_expr(c, local, e->lhs);
        result = check_unary(c, e, operand);
        break;
    }

    case SN_EXPR_BINARY: {
        if (e->op == SN_TOK_QUESTION) {
            /* Ternary `cond ? then : else`. parse_expr.c's parse_expr()
             * encodes this as a BINARY QUESTION node whose rhs is itself a
             * BINARY COLON node pairing the two branches — reusing the
             * binary slots rather than a dedicated AST node (see its
             * comment: "Modelled as a one-armed match-free conditional
             * using the binary slots"). QUESTION/COLON are therefore never
             * real operators here and must not reach check_binary() below,
             * which has no case for either and would report a spurious
             * "operator `:` cannot be applied" (this was, until now, true
             * of every ternary in the corpus, single- or multi-line).
             * SN_EXPR_UNARY with op SN_TOK_QUESTION is the unrelated
             * postfix error-propagation form, handled by check_unary(). */
            SnTypeRep *cond_ty = sn_check_expr(c, local, e->lhs);
            if (!is_error(cond_ty) && !sn_type_is_any(cond_ty) &&
                cond_ty->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, e->lhs->span,
                            "condition must be `bool`");
            }
            SnExpr *pair = e->rhs;
            SnTypeRep *then_ty = sn_check_expr(c, local, pair->lhs);
            SnTypeRep *else_ty = sn_check_expr(c, local, pair->rhs);
            pair->resolved_type = sn_type_error(c->types);
            if (is_error(then_ty) || is_error(else_ty)) {
                result = sn_type_error(c->types);
            } else if (sn_type_is_any(then_ty)) {
                result = else_ty;
            } else if (sn_type_is_any(else_ty)) {
                result = then_ty;
            } else if (then_ty == else_ty) {
                result = then_ty;
            } else if (!types_clash(c, then_ty, else_ty)) {
                result = else_ty; /* then_ty is a subtype of else_ty */
            } else if (!types_clash(c, else_ty, then_ty)) {
                result = then_ty; /* else_ty is a subtype of then_ty */
            } else {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BINARY_TYPE_MISMATCH, e->span,
                            "ternary branches have incompatible types (no implicit "
                            "numeric promotion — branches must match, or one must be "
                            "a subtype of the other)");
                result = sn_type_error(c->types);
            }
            break;
        }
        SnTypeRep *lt = sn_check_expr(c, local, e->lhs);
        SnTypeRep *rt = sn_check_expr(c, local, e->rhs);
        result = check_binary(c, e, lt, rt);
        break;
    }

    case SN_EXPR_ASSIGN: {
        SnSymbol *target_sym = resolve_value_symbol(c, local, e->lhs);
        SnTypeRep *target_ty = e->lhs->resolved_type;
        SnTypeRep *value_ty = sn_check_expr(c, local, e->rhs);
        value_ty = adopt_literal_type(c, e->rhs, value_ty, target_ty);

        if (target_sym && !is_mutable_symbol(c, target_sym)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_IMMUTABLE_REASSIGN, e->span,
                        "`%s` is an immutable `let` binding and cannot be reassigned",
                        e->lhs->text ? e->lhs->text : "<expr>");
        } else if (types_clash(c, value_ty, target_ty)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                        "cannot assign this value's type to `%s`",
                        e->lhs->text ? e->lhs->text : "<expr>");
        }
        result = target_ty;
        break;
    }

    case SN_EXPR_CAST:
        sn_check_expr(c, local, e->lhs);
        result = sn_check_resolve_type(c, e->type); /* trusts the annotation */
        break;

    case SN_EXPR_IS:
        sn_check_expr(c, local, e->lhs);
        sn_check_resolve_type(c, e->type); /* validated to exist; result is always bool */
        result = sn_type_bool(c->types);
        break;

    case SN_EXPR_ARRAY: {
        if (e->args.len == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNTYPED_VARIABLE, e->span,
                        "an empty array literal carries no element type; annotate the "
                        "binding (e.g. `var xs: Array<T> = []`)");
            result = sn_type_error(c->types);
            break;
        }
        SnExpr *first = SN_LIST_AT(e->args, SnExpr, 0);
        SnTypeRep *elem_ty = sn_check_expr(c, local, first);
        for (size_t i = 1; i < e->args.len; i++) {
            SnExpr *item = SN_LIST_AT(e->args, SnExpr, i);
            sn_check_expr(c, local, item); /* checked for coverage; not
                                            * cross-validated against elem_ty
                                            * in this pass (documented gap) */
        }
        result = is_error(elem_ty) ? sn_type_error(c->types)
                                   : sn_type_array(c->types, elem_ty);
        break;
    }

    case SN_EXPR_AWAIT: {
        if (!c->in_async_body) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_AWAIT_OUTSIDE_ASYNC, e->span,
                        "`await` is only valid inside an `async func`/`async method`");
        }
        SnTypeRep *inner = sn_check_expr(c, local, e->lhs);
        if (!is_error(inner) && inner->tag == SN_T_NAMED && inner->decl &&
            inner->decl->name == sn_intern_cstr(c->intern, "Task") && inner->nargs >= 1) {
            result = inner->args[0];
        } else {
            result = inner; /* pass-through best-effort; see check.h */
        }
        break;
    }

    case SN_EXPR_LAMBDA: {
        int all_annotated = 1;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = SN_LIST_AT(e->params, SnParam, i);
            if (!p->type) {
                all_annotated = 0;
                break;
            }
        }
        if (!all_annotated) {
            /* Bidirectional inference not implemented — documented gap, no
             * diagnostic (this is a checker limitation, not a user error). */
            result = sn_type_error(c->types);
            break;
        }
        SnScope lambda_scope;
        sn_scope_init(&lambda_scope, c->arena, local);
        SnTypeRep **params =
            e->params.len
                ? (SnTypeRep **)sn_arena_alloc(c->arena, e->params.len * sizeof(SnTypeRep *))
                : NULL;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = SN_LIST_AT(e->params, SnParam, i);
            SnTypeRep *pty = sn_check_resolve_type(c, p->type);
            params[i] = pty;
            SnSymbol *sym = sn_scope_define(&lambda_scope, sn_intern_cstr(c->intern, p->name),
                                            SN_SYM_PARAM, NULL, p->span);
            if (sym) {
                sym->value_type = pty;
            }
        }
        SnTypeRep *ret;
        if (e->value) {
            ret = sn_check_expr(c, &lambda_scope, e->value);
        } else if (e->body) {
            sn_check_stmt(c, &lambda_scope, e->body);
            ret = e->type ? sn_check_resolve_type(c, e->type) : sn_type_unit(c->types);
        } else {
            ret = sn_type_error(c->types);
        }
        result = sn_type_func(c->types, params, (uint32_t)e->params.len, ret);
        break;
    }

    case SN_EXPR_IF: {
        if (e->lhs) {
            sn_check_expr(c, local, e->lhs);
        }
        SnTypeRep *then_ty = e->rhs ? sn_check_expr(c, local, e->rhs) : sn_type_unit(c->types);
        SnTypeRep *else_ty = e->value ? sn_check_expr(c, local, e->value) : sn_type_unit(c->types);
        if (e->body) {
            sn_check_stmt(c, local, e->body);
        }
        if (e->else_body) {
            sn_check_stmt(c, local, e->else_body);
        }
        result = !is_error(then_ty) ? then_ty : else_ty;
        break;
    }
    case SN_EXPR_MATCH: {
        SnTypeRep *target_ty = e->lhs ? sn_check_expr(c, local, e->lhs) : NULL;
        check_match_exhaustiveness(c, target_ty, &e->arms, e->span);
        SnTypeRep *inferred = NULL;
        for (size_t i = 0; i < e->arms.len; i++) {
            SnMatchArm *arm = SN_LIST_AT(e->arms, SnMatchArm, i);
            SnScope arm_scope;
            sn_scope_init(&arm_scope, c->arena, local);
            bind_pattern_names(c, &arm_scope, arm->pattern);
            if (arm->guard) {
                sn_check_expr(c, &arm_scope, arm->guard);
            }
            SnTypeRep *arm_ty = NULL;
            if (arm->value) {
                arm_ty = sn_check_expr(c, &arm_scope, arm->value);
            }
            if (arm->body) {
                sn_check_stmt(c, &arm_scope, arm->body);
            }
            if (arm_ty && !is_error(arm_ty) && !inferred) {
                inferred = arm_ty;
            }
        }
        result = inferred ? inferred : sn_type_unit(c->types);
        break;
    }
    case SN_EXPR_STRUCT_LIT:
        if (e->lhs) {
            sn_check_expr(c, local, e->lhs);
        }
        if (e->rhs) {
            sn_check_expr(c, local, e->rhs);
        }
        for (size_t i = 0; i < e->args.len; i++) {
            sn_check_expr(c, local, SN_LIST_AT(e->args, SnExpr, i));
        }
        result = sn_type_error(c->types);
        break;

    default:
        result = sn_type_error(c->types);
        break;
    }

    e->resolved_type = result;
    return result;
}

static void check_match_exhaustiveness(SnChecker *c, const SnTypeRep *target_ty,
                                       const SnList *arms, SnSpan span) {
    if (!target_ty || is_error(target_ty) || sn_type_is_any(target_ty) || !arms || arms->len == 0) {
        return;
    }
    for (size_t i = 0; i < arms->len; i++) {
        const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, i);
        if (arm->guard) continue;
        if (!arm->pattern || arm->pattern->kind == SN_PAT_WILDCARD ||
            arm->pattern->kind == SN_PAT_BINDING) {
            return; /* Catch-all / wildcard covers all remaining branches */
        }
    }
    if (target_ty->tag == SN_T_NAMED && target_ty->decl && target_ty->decl->decl &&
        target_ty->decl->decl->kind == SN_DECL_ENUM) {
        const SnDecl *enum_decl = target_ty->decl->decl;
        size_t total_variants = enum_decl->members.len;
        if (total_variants == 0) return;

        for (size_t vi = 0; vi < total_variants; vi++) {
            const SnDecl *v = SN_LIST_AT(enum_decl->members, SnDecl, vi);
            int covered = 0;
            for (size_t ai = 0; ai < arms->len; ai++) {
                const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, ai);
                if (arm->guard) continue;
                if (arm->pattern && arm->pattern->kind == SN_PAT_VARIANT &&
                    arm->pattern->name && v->name &&
                    strcmp(arm->pattern->name, v->name) == 0) {
                    covered = 1;
                    break;
                }
            }
            if (!covered) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NON_EXHAUSTIVE_MATCH, span,
                             "non-exhaustive match on `%s`: missing variant `%s`",
                             enum_decl->name, v->name);
                break;
            }
        }
    }
}

/* ── statements ───────────────────────────────────────────────────────────── */

static void bind_pattern_names(SnChecker *c, SnScope *scope, const SnPattern *pat) {
    if (!pat) {
        return;
    }
    switch (pat->kind) {
    case SN_PAT_BINDING: {
        SnSymbol *sym = sn_scope_define(scope, sn_intern_cstr(c->intern, pat->name),
                                        SN_SYM_LOCAL, NULL, pat->span);
        if (sym) {
            /* Real pattern-to-payload typing (matching a variant's declared
             * field types) isn't implemented — documented gap. */
            sym->value_type = sn_type_error(c->types);
        }
        break;
    }
    case SN_PAT_VARIANT:
        for (size_t i = 0; i < pat->subs.len; i++) {
            bind_pattern_names(c, scope, SN_LIST_AT(pat->subs, SnPattern, i));
        }
        break;
    case SN_PAT_WILDCARD:
    case SN_PAT_LITERAL:
        break;
    }
}

void sn_check_stmt(SnChecker *c, SnScope *local, SnStmt *s) {
    switch (s->kind) {
    case SN_STMT_LET:
    case SN_STMT_VAR: {
        SnTypeRep *declared = s->type ? sn_check_resolve_type(c, s->type) : NULL;
        SnTypeRep *init_ty;
        SnTypeRep *final_ty;

        /* `var ids: Array<string> = []` — the annotation IS the element-type
         * source SNOVA100 asks for, so an empty literal under an annotated
         * array binding is not the untyped case the code reports
         * (tests/compile-pass/ArrayLiteralDeclaredType.snova; the genuinely
         * untyped form still fails in
         * tests/compile-fail/array_literal_untyped.snova). Handled here rather
         * than inside SN_EXPR_ARRAY because only the binding knows the
         * annotation — sn_check_expr has no expected-type channel. */
        if (declared && declared->tag == SN_T_ARRAY && s->expr &&
            s->expr->kind == SN_EXPR_ARRAY && s->expr->args.len == 0) {
            s->expr->resolved_type = declared;
            init_ty = declared;
        } else {
            init_ty = s->expr ? sn_check_expr(c, local, s->expr) : NULL;
        }

        if (declared) {
            init_ty = adopt_literal_type(c, s->expr, init_ty, declared);
            if (types_clash(c, init_ty, declared)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_LET_TYPE_MISMATCH, s->span,
                            "`%s`'s declared type does not match its initializer",
                            s->name);
            }
            final_ty = declared;
        } else if (init_ty) {
            if (sn_type_is_any(init_ty)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ANY_IMPLICITLY_INFERRED, s->span,
                             "type `any` cannot be implicitly inferred for `%s`; add an explicit type annotation `let %s: any = ...`",
                             s->name, s->name);
                final_ty = sn_type_error(c->types);
            } else {
                final_ty = init_ty;
            }
        } else {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNTYPED_VARIABLE, s->span,
                        "variable '%s' has no determinable type; add an explicit type "
                        "annotation",
                        s->name);
            final_ty = sn_type_error(c->types);
        }

        SnSymbol *sym = sn_scope_define(local, sn_intern_cstr(c->intern, s->name),
                                        SN_SYM_LOCAL, NULL, s->span);
        if (sym) {
            sym->value_type = final_ty;
            sym->is_mutable = (s->kind == SN_STMT_VAR);
        }
        for (size_t i = 0; i < s->extra_names.len; i++) {
            const char *extra = SN_LIST_AT(s->extra_names, const char, i);
            SnSymbol *esym = sn_scope_define(local, sn_intern_cstr(c->intern, extra),
                                             SN_SYM_LOCAL, NULL, s->span);
            if (esym) {
                esym->value_type = sn_type_error(c->types); /* `<~` multi-target: not modeled */
                esym->is_mutable = (s->kind == SN_STMT_VAR);
            }
        }
        break;
    }

    case SN_STMT_EXPR:
        if (s->expr) {
            sn_check_expr(c, local, s->expr);
        }
        break;

    case SN_STMT_RETURN: {
        SnTypeRep *ret_ty = s->expr ? sn_check_expr(c, local, s->expr) : sn_type_unit(c->types);
        ret_ty = adopt_literal_type(c, s->expr, ret_ty, c->current_return_type);
        if (types_clash(c, ret_ty, c->current_return_type)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_RETURN_TYPE_MISMATCH, s->span,
                        "returned type does not match the function's declared return type");
        }
        break;
    }

    case SN_STMT_IF: {
        if (s->expr) {
            SnTypeRep *cond = sn_check_expr(c, local, s->expr);
            if (!is_error(cond) && !sn_type_is_any(cond) && cond->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, s->expr->span,
                            "condition must be `bool`");
            }
        }
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        if (s->else_br) {
            sn_check_stmt(c, local, s->else_br);
        }
        break;
    }

    case SN_STMT_WHILE: {
        if (s->expr) {
            SnTypeRep *cond = sn_check_expr(c, local, s->expr);
            if (!is_error(cond) && !sn_type_is_any(cond) && cond->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, s->expr->span,
                            "condition must be `bool`");
            }
        }
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, local, s->then_br);
            c->loop_depth--;
        }
        break;
    }

    case SN_STMT_FOR: {
        if (s->expr) {
            sn_check_expr(c, local, s->expr); /* iterable — element typing not modeled */
        }
        SnScope body_scope;
        sn_scope_init(&body_scope, c->arena, local);
        if (s->name) {
            SnSymbol *sym = sn_scope_define(&body_scope, sn_intern_cstr(c->intern, s->name),
                                            SN_SYM_LOCAL, NULL, s->span);
            if (sym) {
                sym->value_type = sn_type_error(c->types); /* documented gap */
            }
        }
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, &body_scope, s->then_br);
            c->loop_depth--;
        }
        break;
    }

    case SN_STMT_BLOCK:
        sn_check_block(c, local, s);
        break;

    case SN_STMT_BREAK:
        if (c->loop_depth == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BREAK_OUTSIDE_LOOP, s->span,
                        "`break` used outside a loop");
        }
        break;
    case SN_STMT_CONTINUE:
        if (c->loop_depth == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONTINUE_OUTSIDE_LOOP, s->span,
                        "`continue` used outside a loop");
        }
        break;

    case SN_STMT_THROW:
        if (s->expr) {
            sn_check_expr(c, local, s->expr);
        }
        break;

    case SN_STMT_DEFER:
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        break;

    case SN_STMT_TRY:
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        for (size_t i = 0; i < s->catches.len; i++) {
            sn_check_stmt(c, local, SN_LIST_AT(s->catches, SnStmt, i));
        }
        if (s->finally_br) {
            sn_check_stmt(c, local, s->finally_br);
        }
        break;

    case SN_STMT_MATCH: {
        SnTypeRep *target_ty = s->expr ? sn_check_expr(c, local, s->expr) : NULL;
        check_match_exhaustiveness(c, target_ty, &s->arms, s->span);
        for (size_t i = 0; i < s->arms.len; i++) {
            SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            SnScope arm_scope;
            sn_scope_init(&arm_scope, c->arena, local);
            bind_pattern_names(c, &arm_scope, arm->pattern);
            if (arm->guard) {
                sn_check_expr(c, &arm_scope, arm->guard);
            }
            if (arm->value) {
                sn_check_expr(c, &arm_scope, arm->value);
            }
            if (arm->body) {
                sn_check_stmt(c, &arm_scope, arm->body);
            }
        }
        break;
    }

    case SN_STMT_PULSAR:
        if (c->in_async_body) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, s->span,
                         "pulsar statements and functions cannot be used inside an async function");
        }
        if (s->expr) {
            /* `pulsar work()` — the one place a direct call to a pulsar
             * function is legal (SNOVA124 forbids it everywhere else).
             * Consumed by the CALL expression's own callee resolution, not
             * cleared here, so it survives exactly one level of `sn_check_expr`
             * recursion into the CALL node itself. */
            c->in_pulsar_launch = 1;
            sn_check_expr(c, local, s->expr);
            c->in_pulsar_launch = 0;
        }
        break;
    }
}

void sn_check_block(SnChecker *c, SnScope *parent, SnStmt *block) {
    SnScope scope;
    sn_scope_init(&scope, c->arena, parent);
    for (size_t i = 0; i < block->stmts.len; i++) {
        sn_check_stmt(c, &scope, SN_LIST_AT(block->stmts, SnStmt, i));
    }
}

/* Conservative "does this statement guarantee a return/throw on every path"
 * check, for SNOVA_MISSING_RETURN. Deliberately narrow: RETURN/THROW, a BLOCK
 * whose last statement qualifies, and an IF whose branches both qualify.
 * Everything else (loops, match/try as statements, ...) is treated as "not
 * guaranteed" even when it may in fact always return — false negatives here
 * only mean a real gap goes unflagged, which is the safe direction; a false
 * positive would reject correct code. */
static int stmt_always_returns(const SnStmt *s) {
    if (!s) {
        return 0;
    }
    switch (s->kind) {
    case SN_STMT_RETURN:
    case SN_STMT_THROW:
        return 1;
    case SN_STMT_BLOCK:
        return s->stmts.len > 0 &&
              stmt_always_returns(SN_LIST_AT(s->stmts, SnStmt, s->stmts.len - 1));
    case SN_STMT_IF:
        return s->else_br && stmt_always_returns(s->then_br) &&
              stmt_always_returns(s->else_br);
    case SN_STMT_MATCH:
        /* Every arm has an explicit `-> return ...`/`-> throw ...` body — an
         * arm written as a bare value (`0 -> "zero"`) is only a return in
         * match-as-EXPRESSION position, which is a different AST node
         * (SN_EXPR_MATCH); here it would be a statement whose value is
         * discarded, so it does not count. */
        if (s->arms.len == 0) {
            return 0;
        }
        for (size_t i = 0; i < s->arms.len; i++) {
            const SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            if (!arm->body || !stmt_always_returns(arm->body)) {
                return 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

/* SNOVA122's shape rule (docs/snovalang-diagnostics.md, mirrored from
 * crates/snovalang/src/native/selfcheck/pulsar.rs): a pulsar function
 * returns `unit`, `Channel<T>`, or `Select<T>` — nothing else, since a
 * pulsar is fire-and-forget and has no `Task<T>` to hand back a value
 * through. NULL/error types are let through so this never doubles up on a
 * diagnostic sn_check_resolve_type already emitted. */
static int is_allowed_pulsar_return(SnChecker *c, const SnTypeRep *rt) {
    if (!rt || is_error(rt)) {
        return 1;
    }
    if (rt->tag == SN_T_UNIT) {
        return 1;
    }
    if (rt->tag == SN_T_NAMED && rt->decl) {
        const char *name = rt->decl->name;
        if (name == sn_intern_cstr(c->intern, "Channel") ||
            name == sn_intern_cstr(c->intern, "Select")) {
            return 1;
        }
    }
    return 0;
}

/* Defines `owner`'s declared type parameters as SN_SYM_TYPE entries in
 * `scope`. A name already present (a method re-declaring its class's `T`)
 * keeps the first binding — shadowing here would only produce a second
 * typevar for the same name, which is indistinguishable in practice. */
static void define_type_params(SnChecker *c, SnScope *scope, const SnDecl *owner) {
    for (size_t i = 0; i < owner->generics.len; i++) {
        const char *name = SN_LIST_AT(owner->generics, const char, i);
        sn_scope_define(scope, sn_intern_cstr(c->intern, name), SN_SYM_TYPE, NULL,
                        owner->span);
    }
}

void sn_check_decl_body(SnChecker *c, const SnDecl *decl) {
    if (!decl->body) {
        return; /* bare signature or @native — accepted; P5's problem */
    }
    if (decl->kind != SN_DECL_FUNC && decl->kind != SN_DECL_METHOD) {
        return;
    }

    /* Type parameters first: the parameter and return types resolved below
     * may mention them (`func try<T, E>(result: Result<T, E>)`). A method
     * also sees its owning type's — `method map(f: (T) -> U): List<U>` inside
     * `class List<T>` names both. */
    SnScope type_params_scope;
    sn_scope_init(&type_params_scope, c->arena, NULL);
    define_type_params(c, &type_params_scope, decl);
    if (c->enclosing_type) {
        define_type_params(c, &type_params_scope, c->enclosing_type);
    }
    c->type_params = &type_params_scope;

    SnScope params_scope;
    sn_scope_init(&params_scope, c->arena, NULL);
    for (size_t i = 0; i < decl->params.len; i++) {
        SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
        SnTypeRep *pty = sn_check_resolve_type(c, p->type);
        SnSymbol *sym = sn_scope_define(&params_scope, sn_intern_cstr(c->intern, p->name),
                                        SN_SYM_PARAM, NULL, p->span);
        if (sym) {
            sym->value_type = pty;
            sym->is_mutable = 0;
        }
    }

    c->current_return_type = sn_check_resolve_type(c, decl->ret);
    if (decl->is_async && decl->is_pulsar) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, decl->span,
                     "function `%s` cannot be declared as both async and pulsar",
                     decl->name ? decl->name : "<fn>");
    }
    if (decl->is_pulsar && !is_allowed_pulsar_return(c, c->current_return_type)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_INVALID_RETURN, decl->span,
                    "pulsar function `%s` must return `unit`, `Channel<T>`, or `Select<T>`",
                    decl->name ? decl->name : "<fn>");
    }
    int is_layer3 = (c->current_package &&
                     (strncmp(c->current_package, "builtin", 7) == 0 ||
                      strncmp(c->current_package, "stdlib", 6) == 0));
    if (decl->vis == SN_VIS_PUBLIC && is_layer3) {
        if (c->current_return_type && sn_type_is_any(c->current_return_type)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ANY_IN_PUBLIC_LIBRARY, decl->span,
                         "public declaration `%s` in Layer 3 library cannot use `any` in its signature", decl->name ? decl->name : "<fn>");
        }
        for (size_t i = 0; i < decl->params.len; i++) {
            SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
            SnTypeRep *pty = sn_check_resolve_type(c, p->type);
            if (pty && sn_type_is_any(pty)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ANY_IN_PUBLIC_LIBRARY, p->span,
                             "public parameter `%s` in Layer 3 library cannot use `any` in its signature", p->name);
            }
        }
    }
    c->in_constructor =
        decl->name && sn_intern_cstr(c->intern, decl->name) ==
                          sn_intern_cstr(c->intern, "constructor");
    c->in_async_body = decl->is_async;
    sn_check_stmt(c, &params_scope, decl->body);
    if (c->current_return_type && !is_error(c->current_return_type) &&
        c->current_return_type->tag != SN_T_UNIT &&
        !stmt_always_returns(decl->body)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_MISSING_RETURN, decl->span,
                    "`%s` is declared to return a value on every path but doesn't",
                    decl->name ? decl->name : "<fn>");
    }
    c->in_constructor = 0;
    c->in_async_body = 0;
    c->current_return_type = NULL;
    c->type_params = NULL;
}
