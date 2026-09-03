/* cmd_build.c — standalone native compilation command. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_build.h"
#include "arena.h"
#include "diag.h"
#include "driver_utils.h"
#include "emit_bc.h"
#include "eval.h"
#include "lex.h"
#include "native_backend.h"
#include "parse.h"
#include "snbc.h"
#include "target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_build(const char *path, const char *out_path, const char *target_override) {
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

    if (diag.error_count > 0) {
        report_errors(&diag, path);
        sn_arena_free(&arena);
        free(src);
        return 1;
    }

    sn_eval_merge_extensions(&arena, &unit);

    SnBCUnit bc;
    if (!sn_emit_bytecode(&arena, &diag, &unit, &bc)) {
        fprintf(stderr, "error: failed to emit bytecode for '%s'\n", path);
        sn_arena_free(&arena);
        free(src);
        return 1;
    }

    SnTargetInfo target;
    if (target_override && target_override[0]) {
        sn_target_init_default(&target);
        sn_target_parse_triple(&target, target_override);
    } else {
        target = sn_target_get_active();
    }

    char default_out[1024];
    if (!out_path || !out_path[0]) {
        snprintf(default_out, sizeof(default_out), "%s", path);
        char *dot = strrchr(default_out, '.');
        if (dot && strcmp(dot, ".snova") == 0) {
            *dot = '\0';
        } else {
            strncat(default_out, ".out", sizeof(default_out) - strlen(default_out) - 1);
        }
        if (target.exe_ext[0] && !strstr(default_out, target.exe_ext)) {
            strncat(default_out, target.exe_ext, sizeof(default_out) - strlen(default_out) - 1);
        }
        out_path = default_out;
    }

    int ok = sn_native_compile(&bc, &target, out_path);
    sn_bcunit_free(&bc);
    sn_arena_free(&arena);
    free(src);

    if (!ok) {
        fprintf(stderr, "error: native build failed for '%s'\n", path);
        return 1;
    }
    printf("Compiled %s -> %s (target: %s)\n", path, out_path, target.triple);
    return 0;
}
