/* cmd_run.c — execution command for single-file and projects. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_run.h"
#include "arena.h"
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

int cmd_run_project(const char *path) {
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
    sn_pkggraph_scan_root(&graph, proj.source_root);

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
                sn_list_push(&arena, &merged.decls, unit.decls.items[i]);
            }

            sn_diag_set_file(&diag, outer);
        }
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
