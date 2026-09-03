#include "resolve.h"

#include <string.h>

#include "lex.h"
#include "parse.h"

/* Option/Result's real package (see resolve.h note 3). */
#define SN_PRELUDE_PACKAGE "builtin.types.Types"

void sn_resolver_init(SnResolver *r, SnArena *a, SnInternTable *it,
                       SnDiagSink *diag, SnPackageGraph *graph,
                       SnTypeTable *types) {
    r->arena = a;
    r->intern = it;
    r->diag = diag;
    r->graph = graph;
    r->types = types;
    r->prelude_scope = NULL;
    r->packages = NULL;
    r->type_scopes = NULL;
    r->current_origin = NULL;
}

SnScope *sn_resolver_package_scope(const SnResolver *r, const char *package_name) {
    for (SnPackageScopeEntry *e = r->packages; e; e = e->next) {
        if (e->package_name == package_name) {
            return e->scope;
        }
    }
    return NULL;
}

/* An import names either a whole package or PACKAGE.SYMBOL — package.c's
 * resolve_import_target comment documents the same ambiguity for cycle
 * detection/SNOVA050 ("the symbol spelling is the common one in real
 * projects"). Every import-scope lookup in this file needs the same
 * longest-match behavior: try the import string outright as a package name,
 * then drop trailing segments until one names a real, collected package.
 * Without this, `import a.b.RealSymbol` (a.b is the real package) never
 * actually brought RealSymbol into scope — only the bare `import a.b` form
 * worked, silently, even though package.c's own linking phase already
 * accepts both spellings as valid. Returns NULL (an uninterned string, not
 * a valid scope key) when no prefix is a collected package at all. */
static const char *resolve_import_package_name(const SnResolver *r, const char *imp_pkg) {
    if (sn_resolver_package_scope(r, imp_pkg)) {
        return imp_pkg;
    }
    char prefix[2048];
    size_t n = strlen(imp_pkg);
    if (n >= sizeof(prefix)) {
        return NULL;
    }
    memcpy(prefix, imp_pkg, n + 1u);
    for (;;) {
        char *dot = strrchr(prefix, '.');
        if (!dot) {
            return NULL;
        }
        *dot = '\0';
        const char *candidate = sn_intern_cstr(r->intern, prefix);
        if (sn_resolver_package_scope(r, candidate)) {
            return candidate;
        }
    }
}

static SnScope *resolve_import_scope(const SnResolver *r, const char *imp_pkg) {
    const char *real_pkg = resolve_import_package_name(r, imp_pkg);
    return real_pkg ? sn_resolver_package_scope(r, real_pkg) : NULL;
}

SnScope *sn_resolver_type_scope(const SnResolver *r, const SnDecl *type_decl) {
    for (SnTypeScopeEntry *e = r->type_scopes; e; e = e->next) {
        if (e->type_decl == type_decl) {
            return e->member_scope;
        }
    }
    return NULL;
}

static SnScope *package_scope_find_or_create(SnResolver *r, const char *name) {
    SnScope *s = sn_resolver_package_scope(r, name);
    if (s) {
        return s;
    }
    s = (SnScope *)sn_arena_alloc(r->arena, sizeof(SnScope));
    sn_scope_init(s, r->arena, NULL);

    SnPackageScopeEntry *e =
        (SnPackageScopeEntry *)sn_arena_alloc(r->arena, sizeof(SnPackageScopeEntry));
    e->package_name = name;
    e->scope = s;
    e->next = r->packages;
    r->packages = e;
    return s;
}

static SnScope *type_scope_create(SnResolver *r, const SnDecl *decl) {
    SnScope *s = (SnScope *)sn_arena_alloc(r->arena, sizeof(SnScope));
    sn_scope_init(s, r->arena, NULL);

    SnTypeScopeEntry *e =
        (SnTypeScopeEntry *)sn_arena_alloc(r->arena, sizeof(SnTypeScopeEntry));
    e->type_decl = decl;
    e->member_scope = s;
    e->next = r->type_scopes;
    r->type_scopes = e;
    return s;
}

/* ── declaration collection ───────────────────────────────────────────────── */

static int top_level_symbol_kind(SnDeclKind k, SnSymbolKind *out) {
    switch (k) {
    case SN_DECL_CLASS:
    case SN_DECL_STRUCT:
    case SN_DECL_ENUM:
    case SN_DECL_INTERFACE:
    case SN_DECL_TYPEALIAS:
        *out = SN_SYM_TYPE;
        return 1;
    case SN_DECL_FUNC:
        *out = SN_SYM_FUNC;
        return 1;
    case SN_DECL_CONST:
        *out = SN_SYM_CONST;
        return 1;
    default:
        return 0; /* METHOD/FIELD/VARIANT never appear at top level;
                   * EXTENSION is handled by the caller separately */
    }
}

static int member_symbol_kind(SnDeclKind k, SnSymbolKind *out) {
    switch (k) {
    case SN_DECL_METHOD:
    /* Extension members (both `extension X { func f() }` and the
     * single-declaration `extension X.f()` sugar) are parsed as
     * SN_DECL_FUNC, never SN_DECL_METHOD — parse_extension() always
     * builds a plain func decl for the member. Without this case,
     * collect_member() silently dropped every extension method (the
     * `!via_extension` FUNC-in-type-body diagnostic above is the only
     * other place SN_DECL_FUNC meets a member scope, and it already
     * reports its own error), so no `extension` block or single-decl
     * method — including Option/Result's isSome()/unwrap()/map() —
     * was ever actually resolvable as a member. */
    case SN_DECL_FUNC:
        *out = SN_SYM_METHOD;
        return 1;
    case SN_DECL_FIELD:
        *out = SN_SYM_FIELD;
        return 1;
    case SN_DECL_CONST:
        *out = SN_SYM_CONST;
        return 1;
    default:
        return 0;
    }
}

static int decl_kind_has_member_scope(SnDeclKind k) {
    return k == SN_DECL_CLASS || k == SN_DECL_STRUCT || k == SN_DECL_INTERFACE ||
           k == SN_DECL_ENUM;
}

static char *read_whole_file(SnArena *a, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)sn_arena_alloc(a, (size_t)n + 1u);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* sn_scope_define() plus the provenance stamp — see SnSymbol.origin. Every
 * definition made while walking a file must go through here, or a diagnostic
 * raised for that symbol later has no file to be measured against. */
static SnSymbol *define_with_origin(SnResolver *r, SnScope *scope, const char *name,
                                     SnSymbolKind kind, const SnDecl *decl,
                                     SnSpan span) {
    SnSymbol *sym = sn_scope_define(scope, name, kind, decl, span);
    if (sym) {
        sym->origin = r->current_origin;
    }
    return sym;
}

static void collect_member(SnResolver *r, SnScope *member_scope, const SnDecl *owner,
                            const SnDecl *m, const char *via_extension) {
    /* `func` belongs at the top level or in an `extension` body; inside a
     * class/struct/interface it must be `method`. Checked before
     * member_symbol_kind so it is reported whether or not a top-level-shaped
     * declaration has a member symbol kind at all. */
    if (!via_extension && m->kind == SN_DECL_FUNC) {
        sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_FUNC_IN_TYPE_BODY, m->span,
                     "`func` is not valid inside a class body -- use `method`.");
    }

    SnSymbolKind msk;
    if (!member_symbol_kind(m->kind, &msk)) {
        return;
    }
    const char *mname = sn_intern_cstr(r->intern, m->name);

    /* A private backing field and its same-named public accessor method
     * (`private let path: Path` + `method path(): Path { return path }`,
     * both real, in builtin/FileSystem.snova's `File`) is a legitimate,
     * idiomatic pattern — not a collision. Found while running this
     * collector against the real corpus (2026-07-25); not one of plan.md
     * §7's ratified ambiguities, so treated as its own narrow rule: a
     * FIELD/METHOD pair sharing a name is allowed, and whichever was
     * collected first wins the name in the member scope (the practical
     * effect: within-class references to the bare name still resolve to
     * *something* real; disambiguating "the field" from "a call to the
     * method" needs call-site syntax, which is check.c's job, not this
     * scope table's). Any other same-kind collision remains a real error. */
    SnSymbol *existing = sn_scope_lookup_local(member_scope, mname);
    if (existing && ((existing->kind == SN_SYM_FIELD && msk == SN_SYM_METHOD) ||
                      (existing->kind == SN_SYM_METHOD && msk == SN_SYM_FIELD))) {
        return;
    }

    SnSymbol *msym = define_with_origin(r, member_scope, mname, msk, m, m->span);
    if (!msym) {
        SnSymbol *existing_m = sn_scope_lookup_local(member_scope, mname);
        if (existing_m && existing_m->kind == msk &&
            existing_m->origin && r->current_origin &&
            existing_m->origin->path && r->current_origin->path &&
            strcmp(existing_m->origin->path, r->current_origin->path) != 0) {
            return;
        }
        if (via_extension) {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s` (via extension)",
                         m->name, owner->name);
        } else {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL, m->span,
                         "`%s` is already declared in `%s`", m->name, owner->name);
        }
    }
}

typedef struct ExtEntry ExtEntry;
struct ExtEntry {
    const SnDecl *decl; /* SN_DECL_EXTENSION */
    const char *package;
    const SnDiagFile *origin; /* file the extension block was parsed from */
    ExtEntry *next;
};

/* Merges one `extension` block's members into the same-package type it
 * targets. Silently does nothing when the target isn't a type of that package
 * — there is no corpus evidence of cross-package extension, so the whole
 * graph is not searched for one. */
static void collect_extension_members(SnResolver *r, const ExtEntry *ee) {
    const char *target_name = sn_intern_cstr(r->intern, ee->decl->name);
    SnScope *pkg_scope = sn_resolver_package_scope(r, ee->package);
    SnSymbol *target_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, target_name) : NULL;
    if (!target_sym || target_sym->kind != SN_SYM_TYPE) {
        return;
    }
    SnScope *member_scope = sn_resolver_type_scope(r, target_sym->decl);
    if (!member_scope) {
        return;
    }
    for (size_t j = 0; j < ee->decl->members.len; j++) {
        SnDecl *m = SN_LIST_AT(ee->decl->members, SnDecl, j);
        collect_member(r, member_scope, ee->decl, m, ee->decl->name);
    }
}

size_t sn_resolver_collect(SnResolver *r) {
    size_t skipped = 0;
    ExtEntry *pending_ext = NULL;

    for (SnPackageNode *node = r->graph->nodes; node; node = node->next) {
        SnScope *pkg_scope = package_scope_find_or_create(r, node->name);

        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            /* Multi-section files (resolve.h note 1): the same physical path
             * appears under more than one SnPackageFile across the graph.
             * O(n^2) in file count, fine at this corpus's scale. */
            int occurrences = 0;
            for (SnPackageNode *n2 = r->graph->nodes; n2; n2 = n2->next) {
                for (SnPackageFile *pf2 = n2->files; pf2; pf2 = pf2->next) {
                    if (strcmp(pf2->path, pf->path) == 0) {
                        occurrences++;
                    }
                }
            }
            if (occurrences > 1) {
                skipped++;
                continue;
            }

            size_t len = 0;
            char *src = read_whole_file(r->arena, pf->path, &len);
            if (!src) {
                continue;
            }

            /* Arena-allocated so it outlives this loop: every symbol defined
             * below keeps a pointer to it, and check.c reads it much later. */
            SnDiagFile *origin = (SnDiagFile *)sn_arena_alloc(r->arena, sizeof(SnDiagFile));
            origin->path = pf->path;
            origin->src = src;
            origin->src_len = len;
            r->current_origin = origin;
            SnDiagFile outer = sn_diag_set_file(r->diag, *origin);

            SnTokenVec toks;
            memset(&toks, 0, sizeof(toks));
            sn_lex(r->arena, r->diag, src, len, &toks);

            SnUnit unit;
            sn_parse(r->arena, r->diag, &toks, &unit);

            for (size_t i = 0; i < unit.decls.len; i++) {
                SnDecl *d = SN_LIST_AT(unit.decls, SnDecl, i);

                if (d->kind == SN_DECL_EXTENSION) {
                    ExtEntry *ee = (ExtEntry *)sn_arena_alloc(r->arena, sizeof(ExtEntry));
                    ee->decl = d;
                    ee->package = node->name;
                    ee->origin = origin;
                    ee->next = pending_ext;
                    pending_ext = ee;
                    continue;
                }

                /* `method` needs an owning type; at the top level it must be
                 * `func`. Reported before the symbol is defined so the name
                 * still enters scope and callers don't cascade. */
                if (d->kind == SN_DECL_METHOD) {
                    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_METHOD_AT_TOP_LEVEL,
                                 d->span,
                                 "`method` is not valid at the top level -- use `func` "
                                 "for module-level functions.");
                }

                SnSymbolKind sk;
                if (!top_level_symbol_kind(d->kind, &sk)) {
                    continue;
                }

                const char *name = sn_intern_cstr(r->intern, d->name);
                SnSymbol *sym = define_with_origin(r, pkg_scope, name, sk, d, d->span);
                if (!sym) {
                    SnSymbol *existing = sn_scope_lookup_local(pkg_scope, name);
                    if (existing && existing->kind == sk &&
                        existing->decl && d && existing->decl->kind == d->kind &&
                        existing->origin && origin && existing->origin->path && origin->path &&
                        strcmp(existing->origin->path, origin->path) != 0) {
                        /* Duplicate identical declaration from redundant dependency roots; reuse existing */
                        continue;
                    }
                    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL,
                                 d->span, "`%s` is already declared in package `%s`",
                                 d->name, node->name);
                    continue;
                }

                if (decl_kind_has_member_scope(d->kind)) {
                    SnScope *member_scope = type_scope_create(r, d);
                    for (size_t j = 0; j < d->members.len; j++) {
                        SnDecl *m = SN_LIST_AT(d->members, SnDecl, j);
                        collect_member(r, member_scope, d, m, NULL);
                    }
                    for (size_t j = 0; j < d->variants.len; j++) {
                        SnDecl *v = SN_LIST_AT(d->variants, SnDecl, j);
                        const char *vname = sn_intern_cstr(r->intern, v->name);
                        SnSymbol *vsym = define_with_origin(r, member_scope, vname,
                                                           SN_SYM_VARIANT, v, v->span);
                        if (!vsym) {
                            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_DUPLICATE_DECL,
                                         v->span, "`%s` is already declared in `%s`",
                                         v->name, d->name);
                        }
                    }
                }
            }

            sn_diag_set_file(r->diag, outer);
            r->current_origin = NULL;
        }
    }

    /* Pass B: extension members, now that every type in this run has a
     * member scope to merge into. This is how Option/Result actually get
     * isSome()/unwrap()/map()/... — the enum body only has variants. */
    for (ExtEntry *ee = pending_ext; ee; ee = ee->next) {
        r->current_origin = ee->origin;
        SnDiagFile outer = sn_diag_set_file(r->diag, *ee->origin);
        collect_extension_members(r, ee);
        sn_diag_set_file(r->diag, outer);
        r->current_origin = NULL;
    }

    return skipped;
}


/* Option/Result's own variants (Some/None/Ok/Err) are also usable with no
 * import — builtin/Types.snova's own doc comment says so explicitly
 * ("construction (Some(v), None, Ok(v), Err(e))... is usable in every
 * Snovalang program with no import"). Found while testing variant
 * construction (`Some(1)`) against the checker: registering only the type
 * names Option/Result in the prelude scope left their variants reachable
 * only via `Option.Some(...)`, which is not how the corpus/spec actually
 * uses them. */
static void add_variants_to_prelude(SnResolver *r, SnSymbol *type_sym) {
    SnScope *member_scope = sn_resolver_type_scope(r, type_sym->decl);
    if (!member_scope) {
        return;
    }
    for (size_t i = 0; i < member_scope->nbuckets; i++) {
        for (SnSymbol *sym = member_scope->buckets[i]; sym; sym = sym->next) {
            if (sym->kind == SN_SYM_VARIANT) {
                sn_scope_define(r->prelude_scope, sym->name, sym->kind, sym->decl,
                                sym->span);
            }
        }
    }
}

/* Copies every top-level TYPE declared by `package_name` into the prelude
 * scope. A name already present is left alone: the packages are folded in
 * source order below, so the first one to claim a name keeps it. */
static void add_package_types_to_prelude(SnResolver *r, const char *package_name) {
    SnScope *src = sn_resolver_package_scope(r, sn_intern_cstr(r->intern, package_name));
    if (!src) {
        return; /* not among the roots scanned this run — the names simply
                 * need an explicit import, which is not a crash */
    }
    for (size_t i = 0; i < src->nbuckets; i++) {
        for (SnSymbol *sym = src->buckets[i]; sym; sym = sym->next) {
            if (sym->kind != SN_SYM_TYPE) {
                continue;
            }
            if (sn_scope_lookup_local(r->prelude_scope, sym->name)) {
                continue;
            }
            sn_scope_define(r->prelude_scope, sym->name, sym->kind, sym->decl, sym->span);
        }
    }
}

void sn_resolver_build_prelude(SnResolver *r) {
    r->prelude_scope = (SnScope *)sn_arena_alloc(r->arena, sizeof(SnScope));
    sn_scope_init(r->prelude_scope, r->arena, NULL);

    /* The prelude's membership is already decided elsewhere in this repo, and
     * this mirrors that decision rather than inventing one:
     * crates/snovalang/src/modules/resolve.rs folds `builtin.datetime.DateTime`
     * and `builtin.types.Types` in unconditionally (BUILTIN_PRELUDE_PACKAGES)
     * and `builtin.collections.Collections` on demand
     * (COLLECTIONS_PRELUDE_PACKAGE), and
     * specs/20260711/builtin-prelude-types/ plus
     * tests/compile-pass/packages/builtin_prelude_no_import.snova state the
     * same three.
     *
     * snovac folds Collections in unconditionally instead of on demand: the
     * "only when referenced" rule exists there because folding it in injects
     * its SOURCE into the compiled bundle, which the frozen Stage 0
     * type-checker cannot handle for its generic methods. Here the prelude is
     * a scope, not a source fold — nothing is injected, so a name that is
     * never referenced simply is never looked up. What that package declares
     * is unchanged either way.
     *
     * Free functions are NOT folded in: `parseInt` lives in
     * `builtin.strings.Strings`, which no prelude list names, so calling it
     * without an import stays an error. */
    add_package_types_to_prelude(r, SN_PRELUDE_PACKAGE);
    add_package_types_to_prelude(r, "builtin.datetime.DateTime");
    add_package_types_to_prelude(r, "builtin.collections.Collections");

    /* Option/Result additionally publish their VARIANTS into the prelude. */
    SnScope *types_pkg =
        sn_resolver_package_scope(r, sn_intern_cstr(r->intern, SN_PRELUDE_PACKAGE));
    if (types_pkg) {
        const char *const WITH_VARIANTS[] = {"Option", "Result"};
        for (size_t i = 0; i < sizeof(WITH_VARIANTS) / sizeof(WITH_VARIANTS[0]); i++) {
            SnSymbol *sym =
                sn_scope_lookup_local(types_pkg, sn_intern_cstr(r->intern, WITH_VARIANTS[i]));
            if (sym) {
                add_variants_to_prelude(r, sym);
            }
        }
    }

    /* Ensure fundamental Option / Result variants are unconditionally available */
    static const char *const BUILTIN_VARIANTS[] = {"Some", "None", "Ok", "Err"};
    for (size_t i = 0; i < sizeof(BUILTIN_VARIANTS) / sizeof(BUILTIN_VARIANTS[0]); i++) {
        const char *vname = sn_intern_cstr(r->intern, BUILTIN_VARIANTS[i]);
        if (!sn_scope_lookup_local(r->prelude_scope, vname)) {
            SnDecl *vd = (SnDecl *)sn_arena_calloc(r->arena, sizeof(SnDecl));
            vd->kind = SN_DECL_VARIANT;
            vd->name = vname;
            if (strcmp(BUILTIN_VARIANTS[i], "Some") == 0 ||
                strcmp(BUILTIN_VARIANTS[i], "Ok") == 0 ||
                strcmp(BUILTIN_VARIANTS[i], "Err") == 0) {
                SnParam *p = (SnParam *)sn_arena_calloc(r->arena, sizeof(SnParam));
                p->name = sn_intern_cstr(r->intern, "value");
                sn_list_push(r->arena, &vd->params, p);
            }
            SnSymbol *sym = (SnSymbol *)sn_arena_calloc(r->arena, sizeof(SnSymbol));
            sym->kind = SN_SYM_VARIANT;
            sym->name = vname;
            sym->decl = vd;
            sn_scope_define(r->prelude_scope, vname, SN_SYM_VARIANT, vd, (SnSpan){0, 0, 0, 0});
        }
    }
}

/* ── name resolution ──────────────────────────────────────────────────────── */

static SnTypeRep *primitive_by_name(SnResolver *r, const char *iname) {
    SnInternTable *it = r->intern;
    if (iname == sn_intern_cstr(it, "int")) return sn_type_int(r->types);
    if (iname == sn_intern_cstr(it, "string")) return sn_type_string(r->types);
    if (iname == sn_intern_cstr(it, "bool")) return sn_type_bool(r->types);
    if (iname == sn_intern_cstr(it, "unit")) return sn_type_unit(r->types);
    if (iname == sn_intern_cstr(it, "long")) return sn_type_long(r->types);
    if (iname == sn_intern_cstr(it, "double")) return sn_type_double(r->types);
    if (iname == sn_intern_cstr(it, "decimal")) return sn_type_decimal(r->types);
    if (iname == sn_intern_cstr(it, "char")) return sn_type_char(r->types);
    /* Also intrinsic, and also declared by no .snova file anywhere (measured
     * the same way as the seven above): `any`, `float` and `byte` are used in
     * builtin signatures (`func printline(value: any)`) and are on the Rust
     * frontend's builtin-type list too — see
     * crates/snovalang/src/native/selfcheck/static_facts.rs. Without them the
     * checker reported SNOVA0027 for every builtin signature mentioning one. */
    if (iname == sn_intern_cstr(it, "any")) return sn_type_any(r->types);
    if (iname == sn_intern_cstr(it, "float")) return sn_type_float(r->types);
    if (iname == sn_intern_cstr(it, "byte")) return sn_type_byte(r->types);
    return NULL;
}

/* `Int`/`String`/... — accepted, mapped to the primitive, and warned about.
 * Returns NULL when `iname` is not one of these. Tried only after every real
 * lookup has failed, so a package that genuinely declares a type named `Int`
 * still wins.
 *
 * The first six are exactly the pairs
 * crates/snovalang/src/native/selfcheck/mod.rs warns about under SNOVA011.
 * The last four complete the set over the primitives snovac actually has a
 * tag for: `Float` alone accounts for 13 of the errors this gate attributes to
 * tests/compile-pass/data_class.snova, which writes `public let x: Float`.
 * The Rust table stops at six only because it is a hardcoded list, not because
 * the capitalized spelling means something else there — so the same advice is
 * given, rather than SNOVA0027 for a type the shipped `snova check` accepts. */
static SnTypeRep *legacy_primitive_alias(SnResolver *r, const char *iname,
                                          const char *name, SnSpan span) {
    static const char *const ALIASES[][2] = {
        {"String", "string"}, {"Int", "int"},   {"Long", "long"},
        {"Bool", "bool"},     {"Unit", "unit"}, {"Decimal", "decimal"},
        {"Float", "float"},   {"Double", "double"},
        {"Byte", "byte"},     {"Char", "char"},
    };
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (iname != sn_intern_cstr(r->intern, ALIASES[i][0])) {
            continue;
        }
        sn_diag_emit(r->diag, SN_DIAG_WARNING, SNOVA_LEGACY_TYPE_SPELLING, span,
                    "Prefer `%s` over `%s` in native Snovalang.", ALIASES[i][1], name);
        return primitive_by_name(r, sn_intern_cstr(r->intern, ALIASES[i][1]));
    }
    return NULL;
}

SnSymbol *sn_resolve_ident(SnResolver *r, const char *current_package,
                            const SnScope *local, const SnDecl *enclosing_type,
                            const SnList *imports, const char *name, SnSpan span) {
    const char *iname = sn_intern_cstr(r->intern, name);

    if (local) {
        SnSymbol *sym = sn_scope_lookup(local, iname);
        if (sym) {
            return sym;
        }
    }

    if (enclosing_type) {
        /* Single-inheritance walk (plan.md's ratified ambiguity 5). The
         * supertype name is looked up in the current package's scope only
         * — no corpus evidence of cross-package inheritance to test against;
         * a `depth` guard stands in for cycle detection since supertypes
         * aren't validated acyclic anywhere before this runs. */
        const SnDecl *cur = enclosing_type;
        int depth = 0;
        while (cur && depth < 64) {
            SnScope *ms = sn_resolver_type_scope(r, cur);
            if (ms) {
                SnSymbol *sym = sn_scope_lookup_local(ms, iname);
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
            const char *super_name = sn_intern_cstr(r->intern, super_ty->name);
            SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
            SnSymbol *super_sym =
                pkg_scope ? sn_scope_lookup_local(pkg_scope, super_name) : NULL;
            cur = (super_sym && super_sym->kind == SN_SYM_TYPE) ? super_sym->decl : NULL;
            depth++;
        }
    }

    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    if (pkg_scope) {
        SnSymbol *sym = sn_scope_lookup_local(pkg_scope, iname);
        if (sym) {
            return sym;
        }
    }

    if (imports) {
        for (size_t i = 0; i < imports->len; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            SnScope *imp_scope = resolve_import_scope(r, imp_pkg);
            if (!imp_scope) {
                continue;
            }
            SnSymbol *sym = sn_scope_lookup_local(imp_scope, iname);
            if (sym) {
                return sym;
            }
        }
    }

    if (r->prelude_scope) {
        SnSymbol *sym = sn_scope_lookup_local(r->prelude_scope, iname);
        if (sym) {
            return sym;
        }
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                "`%s` is not defined in this scope", name);
    return NULL;
}

SnTypeRep *sn_resolve_type_name(SnResolver *r, const char *current_package,
                                 const SnList *imports, const char *name,
                                 SnSpan span) {
    const char *iname = sn_intern_cstr(r->intern, name);

    SnTypeRep *prim = primitive_by_name(r, iname);
    if (prim) {
        return prim;
    }

    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    SnSymbol *sym = pkg_scope ? sn_scope_lookup_local(pkg_scope, iname) : NULL;

    if (!sym && imports) {
        for (size_t i = 0; i < imports->len && !sym; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            SnScope *imp_scope = resolve_import_scope(r, imp_pkg);
            if (imp_scope) {
                sym = sn_scope_lookup_local(imp_scope, iname);
            }
        }
    }

    if (!sym && r->prelude_scope) {
        sym = sn_scope_lookup_local(r->prelude_scope, iname);
    }

    if (sym && sym->kind == SN_SYM_TYPE) {
        /* Bare reference, no generic args threaded through — this is name
         * resolution, not full construction with argument checking (P2.5's
         * job). Callers build `List<int>` themselves via sn_type_named()
         * once `List` and `int` have each resolved independently. */
        return sn_type_named(r->types, sym, NULL, 0);
    }

    /* Not found locally/via import/via prelude: search the whole graph
     * before giving up, to tell "doesn't exist" (SNOVA_UNKNOWN_TYPE) apart
     * from "exists, just not imported" (SNOVA_TYPE_NOT_IMPORTED). */
    for (SnPackageScopeEntry *e = r->packages; e; e = e->next) {
        SnSymbol *found = sn_scope_lookup_local(e->scope, iname);
        if (found && found->kind == SN_SYM_TYPE) {
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_TYPE_NOT_IMPORTED, span,
                        "type `%s` is not imported", name);
            return NULL;
        }
    }

    /* Legacy capitalized spellings resolve to the primitive with a warning,
     * not an error — the same six pairs, and the same SNOVA011 advice, that
     * crates/snovalang/src/native/selfcheck/mod.rs applies. Without this,
     * `func main(): Int` (tests/compile-pass/hello.snova) failed here while
     * the shipped `snova check` accepted it. */
    SnTypeRep *legacy = legacy_primitive_alias(r, iname, name, span);
    if (legacy) {
        return legacy;
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_TYPE, span,
                "Unknown type `%s`. Declare it or import the package that provides it.",
                name);
    return NULL;
}

SnSymbol *sn_resolve_member_path(SnResolver *r, const char *current_package,
                                  const SnScope *local, const SnDecl *enclosing_type,
                                  const SnList *imports, const SnList *segments,
                                  SnSpan span) {
    if (segments->len == 0) {
        return NULL;
    }

    const char *head = SN_LIST_AT(*segments, const char, 0);
    const char *head_i = sn_intern_cstr(r->intern, head);

    /* 1. try as a value first — variable/type-member/package-level, in that
     * order, WITHOUT emitting a diagnostic on failure (a package prefix is
     * still a legitimate possibility; only step 2 failing is a real error). */
    if (local) {
        SnSymbol *sym = sn_scope_lookup(local, head_i);
        if (sym) {
            return sym; /* rest of `segments`: check.c's job, see resolve.h note 4 */
        }
    }
    if (enclosing_type) {
        SnScope *ms = sn_resolver_type_scope(r, enclosing_type);
        if (ms) {
            SnSymbol *sym = sn_scope_lookup_local(ms, head_i);
            if (sym) {
                return sym;
            }
        }
    }
    SnScope *pkg_scope = sn_resolver_package_scope(r, current_package);
    if (pkg_scope) {
        SnSymbol *sym = sn_scope_lookup_local(pkg_scope, head_i);
        if (sym) {
            return sym;
        }
    }
    if (imports) {
        for (size_t i = 0; i < imports->len; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            SnScope *imp_scope = resolve_import_scope(r, imp_pkg);
            if (!imp_scope) {
                continue;
            }
            SnSymbol *sym = sn_scope_lookup_local(imp_scope, head_i);
            if (sym) {
                return sym;
            }
        }
    }
    if (r->prelude_scope) {
        SnSymbol *sym = sn_scope_lookup_local(r->prelude_scope, head_i);
        if (sym) {
            return sym;
        }
    }

    /* 2. try as a package prefix: greedily match the longest run of leading
     * segments that names a real package in the graph, then resolve the
     * next segment (if any) as a top-level symbol of that package. */
    char buf[512];
    size_t n = 0;
    size_t matched_upto = 0;
    const char *matched_pkg = NULL;
    for (size_t i = 0; i < segments->len; i++) {
        const char *seg = SN_LIST_AT(*segments, const char, i);
        size_t sl = strlen(seg);
        if (i > 0 && n + 1u < sizeof(buf)) {
            buf[n++] = '.';
        }
        if (n + sl < sizeof(buf)) {
            memcpy(buf + n, seg, sl);
            n += sl;
        }
        buf[n] = '\0';
        const char *candidate = sn_intern_cstr(r->intern, buf);
        if (sn_pkggraph_find(r->graph, candidate)) {
            matched_upto = i + 1u;
            matched_pkg = candidate;
        }
    }

    /* 2.5. try as an imported package alias: if `head` matches the last
     * segment of an imported package, treat it as that package. `imp_pkg`
     * itself is often PACKAGE.SYMBOL rather than a real package name (the
     * common `import a.b.RealSymbol` spelling), so the scope lookup below
     * needs the real, collected package name — resolve_import_package_name
     * finds it the same way resolve_import_scope does elsewhere in this
     * file. */
    if (!matched_pkg && imports) {
        for (size_t i = 0; i < imports->len; i++) {
            const char *imp_pkg = SN_LIST_AT(*imports, const char, i);
            const char *last = strrchr(imp_pkg, '.');
            const char *stem = last ? last + 1 : imp_pkg;
            if (strcmp(stem, head) == 0) {
                matched_pkg = resolve_import_package_name(r, imp_pkg);
                if (matched_pkg) {
                    matched_upto = 1;
                    break;
                }
            }
        }
    }

    if (matched_pkg) {
        if (matched_upto < segments->len) {
            const char *next_seg = SN_LIST_AT(*segments, const char, matched_upto);
            const char *next_i = sn_intern_cstr(r->intern, next_seg);
            SnScope *matched_scope = sn_resolver_package_scope(r, matched_pkg);
            SnSymbol *sym =
                matched_scope ? sn_scope_lookup_local(matched_scope, next_i) : NULL;
            if (sym) {
                return sym;
            }
            sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                        "`%s` is not defined in this scope", next_seg);
            return NULL;
        }
        /* the whole path names a package with nothing left to resolve to a
         * symbol; not itself an error at this layer. */
        return NULL;
    }

    sn_diag_emit(r->diag, SN_DIAG_ERROR, SNOVA_UNDECLARED_NAME, span,
                "`%s` is not defined in this scope", head);
    return NULL;
}
