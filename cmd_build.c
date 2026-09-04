/* cmd_build.c — standalone native compilation command. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_build.h"
#include "arena.h"
#include "cmd_check.h"
#include "diag.h"
#include "driver_utils.h"
#include "emit_bc.h"
#include "eval.h"
#include "intern.h"
#include "lex.h"
#include "native_backend.h"
#include "package.h"
#include "parse.h"
#include "project.h"
#include "snbc.h"
#include "target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_build(const char *path, const char *out_path,
              const char *target_override) {
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
      strncat(default_out, ".out",
              sizeof(default_out) - strlen(default_out) - 1);
    }
    if (target.exe_ext[0] && !strstr(default_out, target.exe_ext)) {
      strncat(default_out, target.exe_ext,
              sizeof(default_out) - strlen(default_out) - 1);
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

int cmd_build_project(const char *path, const char *out_path,
                      const char *target_override, const char *offline_cache,
                      int include_runtime) {
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
  sn_arena_init(&arena, 8 * 1024 * 1024);
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

  if (diag.error_count > 0) {
    report_errors(&diag, path);
    sn_arena_free(&arena);
    return 1;
  }

  sn_eval_merge_extensions(&arena, &merged);

  SnBCUnit bc;
  if (!sn_emit_bytecode(&arena, &diag, &merged, &bc)) {
    fprintf(stderr, "error: failed to emit bytecode for project '%s'\n", path);
    sn_arena_free(&arena);
    return 1;
  }

  if (proj.cache_root[0]) {
    char cache_file[SNOVAC_PATH_MAX + 64];
    snprintf(cache_file, sizeof(cache_file), "%s/project_bundle.snbc",
             proj.cache_root);
    sn_bcunit_write_file(&bc, cache_file);
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
    snprintf(default_out, sizeof(default_out), "%s/app", path);
    if (target.exe_ext[0] && !strstr(default_out, target.exe_ext)) {
      strncat(default_out, target.exe_ext,
              sizeof(default_out) - strlen(default_out) - 1);
    }
    out_path = default_out;
  }

  int ok;
  if (include_runtime) {
    ok = sn_native_compile_runtime(&graph, &target, out_path);
  } else {
    ok = sn_native_compile(&bc, &target, out_path);
  }
  sn_bcunit_free(&bc);
  sn_arena_free(&arena);

  if (!ok) {
    fprintf(stderr, "error: native build failed for project '%s'\n", path);
    return 1;
  }
  printf("Compiled project %s (%s) -> %s (target: %s)\n", path,
         include_runtime ? "bundled binary + embedded runtime"
                         : "bundled binary dependencies",
         out_path, target.triple);
  return 0;
}
