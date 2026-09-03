/* test_resolve.c — declaration collection + name resolution + prelude.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 step 6
 * verification. Standalone C binary (same rationale as the other test_*.c
 * files: resolve.c has no CLI surface yet).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define mkdir(dir, mode) _mkdir(dir)
#endif

#include "../arena.h"
#include "../diag.h"
#include "../intern.h"
#include "../package.h"
#include "../resolve.h"
#include "../symbol.h"
#include "../types.h"

static int pass = 0;
static int fail = 0;

#define CHECK(name, cond)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            pass++;                                                         \
        } else {                                                             \
            fail++;                                                         \
            printf("FAIL %s\n", name);                                      \
        }                                                                    \
    } while (0)

static void write_file(const char *dir, const char *name, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("FAIL setup: could not create %s\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

static SnSpan zero_span(void) {
    SnSpan s;
    memset(&s, 0, sizeof(s));
    s.line = 1;
    s.col = 1;
    return s;
}

/* One SnArena/SnInternTable/SnTypeTable/SnPackageGraph/SnResolver per
 * scenario keeps fixtures isolated from each other. */
typedef struct {
    SnArena arena;
    SnInternTable intern;
    SnDiagSink diag;
    SnPackageGraph graph;
    SnTypeTable types;
    SnResolver resolver;
} World;

static void world_init(World *w) {
    sn_arena_init(&w->arena, 0);
    sn_intern_init(&w->intern, &w->arena);
    sn_diag_init(&w->diag, "<test>", "", 0);
    sn_pkggraph_init(&w->graph, &w->arena, &w->intern, &w->diag);
    sn_types_init(&w->types, &w->arena);
    sn_resolver_init(&w->resolver, &w->arena, &w->intern, &w->diag, &w->graph,
                     &w->types);
}

static SnList make_imports(World *w, const char *pkg) {
    SnList l;
    memset(&l, 0, sizeof(l));
    sn_list_push(&w->arena, &l, (void *)sn_intern_cstr(&w->intern, pkg));
    return l;
}

/* ── scenarios ────────────────────────────────────────────────────────────── */

static void test_basic_collection_and_package_level_lookup(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/basic", tmp);
    mkdir(dir, 0755);
    write_file(dir, "a.snova",
              "package pkg.a\n\nfunc helper(): int {\n    return 1\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_pkggraph_link(&w.graph);
    size_t skipped = sn_resolver_collect(&w.resolver);
    CHECK("basic: nothing skipped as multi-section", skipped == 0);

    const char *pkg_a = sn_intern_cstr(&w.intern, "pkg.a");
    SnSymbol *sym = sn_resolve_ident(&w.resolver, pkg_a, NULL, NULL, NULL,
                                     "helper", zero_span());
    CHECK("basic: helper resolves at package level", sym != NULL);
    CHECK("basic: resolves to a func symbol", sym != NULL && sym->kind == SN_SYM_FUNC);
    CHECK("basic: no diagnostic emitted for a real name", w.diag.error_count == 0);

    SnSymbol *missing = sn_resolve_ident(&w.resolver, pkg_a, NULL, NULL, NULL,
                                         "doesNotExist", zero_span());
    CHECK("basic: unknown name returns NULL", missing == NULL);
    CHECK("basic: unknown name emits SNOVA_UNDECLARED_NAME", w.diag.error_count == 1);

    sn_arena_free(&w.arena);
}

static void test_local_shadows_package(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/shadow", tmp);
    mkdir(dir, 0755);
    write_file(dir, "s.snova",
              "package pkg.s\n\nfunc x(): int {\n    return 1\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    const char *pkg_s = sn_intern_cstr(&w.intern, "pkg.s");
    SnSymbol *pkg_level = sn_resolve_ident(&w.resolver, pkg_s, NULL, NULL, NULL,
                                           "x", zero_span());
    CHECK("shadow: package-level x found with no local scope", pkg_level != NULL);

    SnScope local;
    sn_scope_init(&local, &w.arena, NULL);
    const char *x_name = sn_intern_cstr(&w.intern, "x");
    SnSymbol *local_sym = sn_scope_define(&local, x_name, SN_SYM_LOCAL, NULL, zero_span());

    SnSymbol *shadowed = sn_resolve_ident(&w.resolver, pkg_s, &local, NULL, NULL,
                                          "x", zero_span());
    CHECK("shadow: local x wins over package-level x",
          shadowed == local_sym && shadowed != pkg_level);

    sn_arena_free(&w.arena);
}

static void test_inheritance_walk(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/inherit", tmp);
    mkdir(dir, 0755);
    write_file(dir, "i.snova",
              "package pkg.i\n\n"
              "class Animal {\n    method speak(): string\n}\n\n"
              "class Dog extends Animal {\n    method bark(): string\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    const char *pkg_i = sn_intern_cstr(&w.intern, "pkg.i");
    SnScope *pkg_scope = sn_resolver_package_scope(&w.resolver, pkg_i);
    CHECK("inherit: package scope exists", pkg_scope != NULL);

    SnSymbol *dog_sym = pkg_scope ? sn_scope_lookup_local(pkg_scope,
                                    sn_intern_cstr(&w.intern, "Dog")) : NULL;
    CHECK("inherit: Dog is collected", dog_sym != NULL);

    SnSymbol *own_method = sn_resolve_ident(&w.resolver, pkg_i, NULL,
                                            dog_sym ? dog_sym->decl : NULL, NULL,
                                            "bark", zero_span());
    CHECK("inherit: Dog's own method resolves", own_method != NULL &&
                                                     own_method->kind == SN_SYM_METHOD);

    SnSymbol *inherited = sn_resolve_ident(&w.resolver, pkg_i, NULL,
                                           dog_sym ? dog_sym->decl : NULL, NULL,
                                           "speak", zero_span());
    CHECK("inherit: Animal's method resolves through Dog (single-inheritance walk)",
          inherited != NULL && inherited->kind == SN_SYM_METHOD);

    sn_arena_free(&w.arena);
}

static void test_import_and_not_imported(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/imports", tmp);
    mkdir(dir, 0755);
    write_file(dir, "a.snova",
              "package pkg.ia\n\nclass Widget {\n    method use(): unit\n}\n");
    write_file(dir, "b.snova", "package pkg.ib\n\nfunc noop(): int {\n    return 0\n}\n");
    write_file(dir, "c.snova", "package pkg.ic\n\nfunc noop(): int {\n    return 0\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    const char *pkg_ia = sn_intern_cstr(&w.intern, "pkg.ia");
    const char *pkg_ib = sn_intern_cstr(&w.intern, "pkg.ib");
    const char *pkg_ic = sn_intern_cstr(&w.intern, "pkg.ic");

    SnList imports_ia = make_imports(&w, "pkg.ia");

    SnTypeRep *found = sn_resolve_type_name(&w.resolver, pkg_ib, &imports_ia,
                                            "Widget", zero_span());
    CHECK("imports: Widget resolves through an explicit import", found != NULL);
    CHECK("imports: resolving via import emits no diagnostic", w.diag.error_count == 0);

    SnTypeRep *not_imported = sn_resolve_type_name(&w.resolver, pkg_ic, NULL,
                                                    "Widget", zero_span());
    CHECK("imports: Widget without the import returns NULL", not_imported == NULL);
    CHECK("imports: emits exactly one diagnostic (SNOVA_TYPE_NOT_IMPORTED)",
          w.diag.error_count == 1);

    (void)pkg_ia; (void)pkg_ib; (void)pkg_ic;
    sn_arena_free(&w.arena);
}

static void test_truly_unknown_type(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/unknown", tmp);
    mkdir(dir, 0755);
    write_file(dir, "u.snova", "package pkg.u\n\nfunc noop(): int {\n    return 0\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    const char *pkg_u = sn_intern_cstr(&w.intern, "pkg.u");
    SnTypeRep *ty = sn_resolve_type_name(&w.resolver, pkg_u, NULL,
                                         "NeverDeclaredAnywhere", zero_span());
    CHECK("unknown: truly nonexistent type returns NULL", ty == NULL);
    CHECK("unknown: emits exactly one diagnostic", w.diag.error_count == 1);

    sn_arena_free(&w.arena);
}

static void test_duplicate_declaration(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/dup", tmp);
    mkdir(dir, 0755);
    write_file(dir, "d.snova",
              "package pkg.dup\n\n"
              "func x(): int {\n    return 1\n}\n\n"
              "func x(): int {\n    return 2\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    CHECK("duplicate: exactly one diagnostic for the second `x`",
          w.diag.error_count == 1);

    const char *pkg_dup = sn_intern_cstr(&w.intern, "pkg.dup");
    SnScope *pkg_scope = sn_resolver_package_scope(&w.resolver, pkg_dup);
    SnSymbol *x = pkg_scope
                      ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w.intern, "x"))
                      : NULL;
    CHECK("duplicate: the first declaration still wins the slot", x != NULL);

    sn_arena_free(&w.arena);
}

static void test_field_method_same_name_is_not_a_collision(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/fieldmethod", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.fm\n\n"
              "class Holder {\n"
              "    private let path: string\n\n"
              "    method path(): string {\n"
              "        return path\n"
              "    }\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    CHECK("field/method same name: no diagnostic (matches builtin/FileSystem.snova's File)",
          w.diag.error_count == 0);

    const char *pkg_fm = sn_intern_cstr(&w.intern, "pkg.fm");
    SnScope *pkg_scope = sn_resolver_package_scope(&w.resolver, pkg_fm);
    SnSymbol *holder =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w.intern, "Holder"))
                  : NULL;
    CHECK("field/method same name: Holder collected", holder != NULL);

    SnSymbol *path_sym = sn_resolve_ident(&w.resolver, pkg_fm, NULL,
                                          holder ? holder->decl : NULL, NULL,
                                          "path", zero_span());
    CHECK("field/method same name: `path` still resolves to something real",
          path_sym != NULL &&
              (path_sym->kind == SN_SYM_FIELD || path_sym->kind == SN_SYM_METHOD));

    sn_arena_free(&w.arena);
}

static void test_extension_merging_and_prelude(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/prelude", tmp);
    mkdir(dir, 0755);
    write_file(dir, "types.snova",
              "package builtin.types.Types\n\n"
              "enum Option {\n    Some(value: int),\n    None,\n}\n\n"
              "extension Option {\n"
              "    method isSome(): bool\n"
              "    method unwrap(): int\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    sn_resolver_build_prelude(&w.resolver);

    const char *builtin_types = sn_intern_cstr(&w.intern, "builtin.types.Types");
    SnScope *pkg_scope = sn_resolver_package_scope(&w.resolver, builtin_types);
    SnSymbol *option_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w.intern, "Option"))
                  : NULL;
    CHECK("prelude: Option is collected in builtin.types.Types", option_sym != NULL);

    SnSymbol *some_variant = sn_resolve_ident(
        &w.resolver, builtin_types, NULL, option_sym ? option_sym->decl : NULL, NULL,
        "Some", zero_span());
    CHECK("prelude: Some (variant) is a member of Option", some_variant != NULL &&
                                                                some_variant->kind ==
                                                                    SN_SYM_VARIANT);

    SnSymbol *unwrap_method = sn_resolve_ident(
        &w.resolver, builtin_types, NULL, option_sym ? option_sym->decl : NULL, NULL,
        "unwrap", zero_span());
    CHECK("prelude: unwrap (from `extension Option`) merged into Option's members",
          unwrap_method != NULL && unwrap_method->kind == SN_SYM_METHOD);

    /* Now resolve `Option` from an unrelated package with NO import at all —
     * this is the actual prelude behavior under test. */
    SnTypeRep *option_ty = sn_resolve_type_name(&w.resolver, sn_intern_cstr(&w.intern,
                                                "pkg.unrelated"),
                                                NULL, "Option", zero_span());
    CHECK("prelude: Option resolves with zero imports via the prelude",
          option_ty != NULL);
    CHECK("prelude: resolving via the prelude emits no diagnostic",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_primitives_bypass_everything(const char *tmp) {
    (void)tmp;
    World w;
    world_init(&w);
    /* Deliberately never scan any root: primitives must resolve with an
     * empty graph and an empty prelude. */

    SnTypeRep *int_ty = sn_resolve_type_name(&w.resolver,
                                              sn_intern_cstr(&w.intern, "irrelevant.package"),
                                              NULL, "int", zero_span());
    CHECK("primitives: int resolves with no packages scanned at all", int_ty != NULL);
    CHECK("primitives: matches types.c's own singleton",
          int_ty == sn_type_int(&w.types));
    CHECK("primitives: emits no diagnostic", w.diag.error_count == 0);

    SnTypeRep *char_ty = sn_resolve_type_name(&w.resolver,
                                              sn_intern_cstr(&w.intern, "irrelevant.package"),
                                              NULL, "char", zero_span());
    CHECK("primitives: char resolves too", char_ty == sn_type_char(&w.types));

    sn_arena_free(&w.arena);
}

static void test_multi_section_file_is_skipped(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/multisection", tmp);
    mkdir(dir, 0755);
    write_file(dir, "e.snova",
              "package pkg.multi1\n\nfunc providerFn(): int {\n    return 1\n}\n\n"
              "package pkg.multi2\n\nfunc consumerFn(): int {\n    return 2\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    size_t skipped = sn_resolver_collect(&w.resolver);

    /* One physical file, two `package` sections -> two SnPackageFile
     * entries in the graph (package.c splits by section) -> the skip
     * counter, which counts entries, is 2 here, not 1. */
    CHECK("multi-section: collect reports both sections as skipped (2 entries)",
          skipped == 2);

    SnScope *s1 = sn_resolver_package_scope(&w.resolver, sn_intern_cstr(&w.intern, "pkg.multi1"));
    SnScope *s2 = sn_resolver_package_scope(&w.resolver, sn_intern_cstr(&w.intern, "pkg.multi2"));
    int has_provider = s1 && sn_scope_lookup_local(s1, sn_intern_cstr(&w.intern, "providerFn"));
    int has_consumer = s2 && sn_scope_lookup_local(s2, sn_intern_cstr(&w.intern, "consumerFn"));
    CHECK("multi-section: neither section's declarations were collected "
          "(documented limitation, not silently wrong)",
          !has_provider && !has_consumer);

    sn_arena_free(&w.arena);
}

static void test_member_path_package_prefix(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/memberpath", tmp);
    mkdir(dir, 0755);
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/deep", dir);
    mkdir(sub, 0755);
    write_file(sub, "c.snova", "package a.b\n\nclass C {\n    method m(): unit\n}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);

    SnList segs;
    memset(&segs, 0, sizeof(segs));
    sn_list_push(&w.arena, &segs, (void *)sn_intern_cstr(&w.intern, "a"));
    sn_list_push(&w.arena, &segs, (void *)sn_intern_cstr(&w.intern, "b"));
    sn_list_push(&w.arena, &segs, (void *)sn_intern_cstr(&w.intern, "C"));

    SnSymbol *found = sn_resolve_member_path(&w.resolver,
                                             sn_intern_cstr(&w.intern, "somewhere.else"),
                                             NULL, NULL, NULL, &segs, zero_span());
    CHECK("member-path: a.b.C resolves via longest package-prefix match",
          found != NULL && found->kind == SN_SYM_TYPE);
    CHECK("member-path: emits no diagnostic on success", w.diag.error_count == 0);

    SnList segs_pkg_only;
    memset(&segs_pkg_only, 0, sizeof(segs_pkg_only));
    sn_list_push(&w.arena, &segs_pkg_only, (void *)sn_intern_cstr(&w.intern, "a"));
    sn_list_push(&w.arena, &segs_pkg_only, (void *)sn_intern_cstr(&w.intern, "b"));
    SnSymbol *bare_pkg = sn_resolve_member_path(&w.resolver,
                                                sn_intern_cstr(&w.intern, "somewhere.else"),
                                                NULL, NULL, NULL, &segs_pkg_only, zero_span());
    CHECK("member-path: a bare package name (nothing left to resolve) returns "
          "NULL without a diagnostic",
          bare_pkg == NULL && w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

int main(void) {
    char tmp[] = "/tmp/snovac_resolve_test_XXXXXX";
    if (!mkdtemp(tmp)) {
        printf("FAIL setup: mkdtemp failed\n");
        return 1;
    }

    test_basic_collection_and_package_level_lookup(tmp);
    test_local_shadows_package(tmp);
    test_inheritance_walk(tmp);
    test_import_and_not_imported(tmp);
    test_truly_unknown_type(tmp);
    test_duplicate_declaration(tmp);
    test_field_method_same_name_is_not_a_collision(tmp);
    test_extension_merging_and_prelude(tmp);
    test_primitives_bypass_everything(tmp);
    test_multi_section_file_is_skipped(tmp);
    test_member_path_package_prefix(tmp);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
