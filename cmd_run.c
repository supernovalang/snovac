/* cmd_run.c — execution command for single-file and projects. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_run.h"
#include "arena.h"
#include "cmd_check.h"
#include "diag.h"
#include "driver_utils.h"
#include "eval.h"
#include "intern.h"
#include "lex.h"
#include "package.h"
#include "parse.h"
#include "project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_run_project_with_cache(const char *path, const char *offline_cache) {
    int check_rc = cmd_check_project(path, 1);
    if (check_rc != 0) {
        return check_rc;
    }

    SnProject proj;
    project_discover(path, &proj);
    if (offline_cache) {
        project_set_offline_cache(&proj, offline_cache);
    }

    SnArena arena;
    sn_arena_init(&arena, 4 * 1024 * 1024);
    SnInternTable intern;
    sn_intern_init(&intern, &arena);
    SnDiagSink diag;
    sn_diag_init(&diag, path, "", 0);

    SnPackageGraph graph;
    sn_pkggraph_init(&graph, &arena, &intern, &diag);
    scan_project_roots(&graph, &proj);

    SnUnit merged;
    memset(&merged, 0, sizeof(merged));

    for (SnPackageNode *node = graph.nodes; node; node = node->next) {
        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            SnDiagFile self = {pf->path, pf->src, pf->src_len};
            SnDiagFile outer = sn_diag_set_file(&diag, self);

            SnTokenVec toks;
            sn_lex(&arena, &diag, pf->src, pf->src_len, &toks);

            SnUnit unit;
            sn_parse(&arena, &diag, &toks, &unit);

            for (size_t i = 0; i < unit.decls.len; i++) {
                SnDecl *d = SN_LIST_AT(unit.decls, SnDecl, i);
                if (d && d->kind == SN_DECL_FUNC && strcmp(d->name, "main") == 0) {
                    /* Libraries NEVER have an entrypoint! Only the application root may execute main() */
                    if (strstr(pf->path, "/.snovalang/deps/") != NULL ||
                        strstr(pf->path, "\\.snovalang\\deps\\") != NULL ||
                        strstr(pf->path, "/snova-std/") != NULL ||
                        strstr(pf->path, "\\snova-std\\") != NULL ||
                        strstr(pf->path, "/snova-") != NULL ||
                        strstr(pf->path, "\\snova-") != NULL) {
                        continue; /* Skip any library dependency's main() */
                    }
                    if (proj.deps_root[0] && strncmp(pf->path, proj.deps_root, strlen(proj.deps_root)) == 0) {
                        continue;
                    }
                }
                sn_list_push(&arena, &merged.decls, d);
            }

            sn_diag_set_file(&diag, outer);
        }
    }

    /* Verify that the project actually has a main() function */
    int has_app_main = 0;
    for (size_t i = 0; i < merged.decls.len; i++) {
        SnDecl *d = SN_LIST_AT(merged.decls, SnDecl, i);
        if (d && d->kind == SN_DECL_FUNC && strcmp(d->name, "main") == 0) {
            has_app_main = 1;
            break;
        }
    }
    if (!has_app_main) {
        fprintf(stderr, "error: no 'main()' function found in project '%s'\n", path);
        sn_arena_free(&arena);
        return 1;
    }

    int rc;
    if (diag.error_count > 0) {
        report_errors(&diag, path);
        rc = 1;
    } else {
        sn_eval_merge_extensions(&arena, &merged);
        int code = sn_eval_run(&arena, &diag, &merged);
        rc = (code < 0) ? 1 : code;
    }

    sn_arena_free(&arena);
    return rc;
}

int cmd_run_project(const char *path) {
    return cmd_run_project_with_cache(path, NULL);
}

int cmd_run(const char *path) {
    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 2;
    }

    SnArena arena;
    sn_arena_init(&arena, 1024 * 1024);

    SnDiagSink diag;
    sn_diag_init(&diag, path, src, len);

    SnTokenVec toks;
    sn_lex(&arena, &diag, src, len, &toks);

    SnUnit unit;
    sn_parse(&arena, &diag, &toks, &unit);

    int rc;
    if (diag.error_count > 0) {
        report_errors(&diag, path);
        rc = 1;
    } else {
        sn_eval_merge_extensions(&arena, &unit);
        int code = sn_eval_run(&arena, &diag, &unit);
        rc = (code < 0) ? 1 : code;
    }

    sn_arena_free(&arena);
    free(src);
    return rc;
}
