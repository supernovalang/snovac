/* cmd_check.c — single-file and project-wide check commands. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_check.h"
#include "driver_utils.h"
#include "lex.h"
#include "parse.h"
#include "project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SnDiagFile begin_symbol_file(SnDiagSink *diag, const SnSymbol *sym) {
    if (!sym->origin) {
        return diag->file;
    }
    return sn_diag_set_file(diag, *sym->origin);
}

static int path_is_own(const SnBodyCheckScope *scope, const char *path) {
    if (!scope || !scope->own_prefix || !scope->own_prefix[0]) return 1;
    if (!path) return 1;
    return strncmp(path, scope->own_prefix, strlen(scope->own_prefix)) == 0;
}

void check_all_bodies(SnChecker *c, SnResolver *resolver, SnPackageGraph *graph,
                      SnArena *arena, const SnBodyCheckScope *scope) {
    for (SnPackageScopeEntry *pe = resolver->packages; pe; pe = pe->next) {
        SnPackageNode *node = sn_pkggraph_find(graph, pe->package_name);
        SnList imports = node ? aggregate_imports(arena, node) : (SnList){0};
        for (size_t i = 0; i < pe->scope->nbuckets; i++) {
            for (SnSymbol *sym = pe->scope->buckets[i]; sym; sym = sym->next) {
                if ((sym->kind == SN_SYM_FUNC || sym->kind == SN_SYM_METHOD) &&
                    sym->decl->body &&
                    path_is_own(scope, sym->origin ? sym->origin->path : NULL)) {
                    c->current_package = pe->package_name;
                    c->current_imports = &imports;
                    c->enclosing_type = NULL;
                    SnDiagFile outer = begin_symbol_file(c->diag, sym);
                    sn_check_decl_body(c, sym->decl);
                    sn_diag_set_file(c->diag, outer);
                }
            }
        }
    }
    for (SnTypeScopeEntry *te = resolver->type_scopes; te; te = te->next) {
        const char *owner_pkg = NULL;
        for (SnPackageScopeEntry *pe = resolver->packages; pe && !owner_pkg; pe = pe->next) {
            for (size_t bi = 0; bi < pe->scope->nbuckets && !owner_pkg; bi++) {
                for (SnSymbol *s = pe->scope->buckets[bi]; s; s = s->next) {
                    if (s->kind == SN_SYM_TYPE && s->decl == te->type_decl) {
                        owner_pkg = pe->package_name;
                        break;
                    }
                }
            }
        }
        SnPackageNode *node = owner_pkg ? sn_pkggraph_find(graph, owner_pkg) : NULL;
        SnList imports = node ? aggregate_imports(arena, node) : (SnList){0};
        for (size_t bi = 0; bi < te->member_scope->nbuckets; bi++) {
            for (SnSymbol *sym = te->member_scope->buckets[bi]; sym; sym = sym->next) {
                if (sym->kind == SN_SYM_METHOD && sym->decl->body &&
                    path_is_own(scope, sym->origin ? sym->origin->path : NULL)) {
                    c->current_package = owner_pkg;
                    c->current_imports = &imports;
                    c->enclosing_type = te->type_decl;
                    SnDiagFile outer = begin_symbol_file(c->diag, sym);
                    sn_check_decl_body(c, sym->decl);
                    sn_diag_set_file(c->diag, outer);
                }
            }
        }
    }
}

void report_import_cycle(SnDiagSink *diag, SnPackageGraph *graph,
                         const SnList *cycle) {
    if (cycle->len == 0) {
        return;
    }
    const char *head = SN_LIST_AT(*cycle, const char, 0);
    SnPackageNode *node = sn_pkggraph_find(graph, head);
    if (!node || !node->files) {
        return;
    }

    char chain[1024];
    size_t used = 0;
    for (size_t i = 0; i < cycle->len; i++) {
        const char *name = SN_LIST_AT(*cycle, const char, i);
        int n = snprintf(chain + used, sizeof(chain) - used, "%s%s",
                         i == 0 ? "" : " -> ", name);
        if (n < 0 || (size_t)n >= sizeof(chain) - used) {
            break;
        }
        used += (size_t)n;
    }

    SnDiagFile origin;
    origin.path = node->files->path;
    origin.src = node->files->src;
    origin.src_len = node->files->src_len;
    SnDiagFile outer = sn_diag_set_file(diag, origin);
    sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_IMPORT_CYCLE,
                 node->files->package_span,
                 "import cycle between packages: %s", chain);
    sn_diag_set_file(diag, outer);
}

int cmd_check(const char *path, int dump) {
    (void)dump;

    char dir[1024];
    dirname_into(path, dir, sizeof(dir));

    SnArena arena;
    sn_arena_init(&arena, 1024 * 1024);
    SnInternTable intern;
    sn_intern_init(&intern, &arena);
    SnDiagSink diag;
    sn_diag_init(&diag, path, "", 0);

    SnPackageGraph graph;
    sn_pkggraph_init(&graph, &arena, &intern, &diag);
    sn_pkggraph_scan_single_file(&graph, path);

    char builtin_dir[1024];
    if (find_builtin_root_for_project(dir, builtin_dir, sizeof(builtin_dir))) {
        sn_pkggraph_scan_root(&graph, builtin_dir);
        sn_pkggraph_load_native_manifest(&graph, builtin_dir);
    }
    sn_pkggraph_link(&graph);

    SnTypeTable types;
    sn_types_init(&types, &arena);
    SnResolver resolver;
    sn_resolver_init(&resolver, &arena, &intern, &diag, &graph, &types);
    sn_resolver_collect(&resolver);
    sn_resolver_build_prelude(&resolver);

    SnChecker checker;
    sn_checker_init(&checker, &arena, &intern, &diag, &resolver, &types);
    SnBodyCheckScope scope = { .own_prefix = path };
    check_all_bodies(&checker, &resolver, &graph, &arena, &scope);

    report_errors(&diag, path);
    int rc = diag.error_count > 0;
    sn_arena_free(&arena);
    return rc;
}

int cmd_check_project(const char *path, int typecheck_bodies) {
    SnProject proj;
    project_discover(path, &proj);

    SnArena arena;
    sn_arena_init(&arena, 4 * 1024 * 1024);
    SnInternTable intern;
    sn_intern_init(&intern, &arena);
    SnDiagSink diag;
    sn_diag_init(&diag, path, "", 0);

    SnPackageGraph graph;
    sn_pkggraph_init(&graph, &arena, &intern, &diag);
    size_t scanned = scan_project_roots(&graph, &proj);
    sn_pkggraph_link(&graph);

    SnList cycle;
    memset(&cycle, 0, sizeof(cycle));
    if (sn_pkggraph_find_cycle(&graph, &cycle)) {
        report_import_cycle(&diag, &graph, &cycle);
    }

    SnTypeTable types;
    sn_types_init(&types, &arena);
    SnResolver resolver;
    sn_resolver_init(&resolver, &arena, &intern, &diag, &graph, &types);
    sn_resolver_collect(&resolver);
    sn_resolver_build_prelude(&resolver);

    if (typecheck_bodies && diag.error_count == 0) {
        SnChecker checker;
        sn_checker_init(&checker, &arena, &intern, &diag, &resolver, &types);
        SnBodyCheckScope scope = { .own_prefix = proj.source_root };
        check_all_bodies(&checker, &resolver, &graph, &arena, &scope);
    }

    if (diag.error_count > 0) {
        fprintf(stderr, "%d error%s across %zu file%s in %s\n",
                diag.error_count, diag.error_count == 1 ? "" : "s",
                scanned, scanned == 1 ? "" : "s", proj.source_root);
    }
    int rc = diag.error_count > 0;
    sn_arena_free(&arena);
    return rc;
}

int cmd_check_parse_project(const char *path, int dump) {
    (void)dump;

    SnProject proj;
    project_discover(path, &proj);

    SnArena arena;
    sn_arena_init(&arena, 4 * 1024 * 1024);
    SnInternTable intern;
    sn_intern_init(&intern, &arena);
    SnDiagSink diag;
    sn_diag_init(&diag, path, "", 0);

    SnPackageGraph graph;
    sn_pkggraph_init(&graph, &arena, &intern, &diag);
    size_t scanned = sn_pkggraph_scan_root(&graph, proj.source_root);
    if (proj.deps_root[0]) {
        sn_pkggraph_scan_root(&graph, proj.deps_root);
    }

    for (SnPackageNode *node = graph.nodes; node; node = node->next) {
        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            SnDiagFile origin;
            origin.path = pf->path;
            origin.src = pf->src;
            origin.src_len = pf->src_len;
            SnDiagFile outer = sn_diag_set_file(&diag, origin);

            SnTokenVec toks;
            memset(&toks, 0, sizeof(toks));
            sn_lex(&arena, &diag, pf->src, pf->src_len, &toks);

            SnUnit unit;
            sn_parse(&arena, &diag, &toks, &unit);

            sn_diag_set_file(&diag, outer);
        }
    }

    if (diag.error_count > 0) {
        fprintf(stderr, "%d error%s across %zu file%s in %s\n",
                diag.error_count, diag.error_count == 1 ? "" : "s",
                scanned, scanned == 1 ? "" : "s", proj.source_root);
    }
    int rc = diag.error_count > 0;
    sn_arena_free(&arena);
    return rc;
}
