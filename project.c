/* project.c — project discovery and multi-file roots scanning. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const MANIFEST_NAMES[] = {"mod.sno", "snova.mod", "snova.sno",
                                             "snova.toml"};

static int find_manifest_dir(const char *start_dir, char *out, size_t out_sz) {
  char cur[SNOVAC_PATH_MAX + 64];
  normalize_path_into(start_dir, cur, sizeof(cur));

  for (int depth = 0; depth < 32; depth++) {
    for (size_t i = 0; i < sizeof(MANIFEST_NAMES) / sizeof(MANIFEST_NAMES[0]);
         i++) {
      char candidate[SNOVAC_PATH_MAX + 128];
      snprintf(candidate, sizeof(candidate), "%s/%s", cur, MANIFEST_NAMES[i]);
      if (path_is_file(candidate)) {
        normalize_path_into(cur, out, out_sz);
        return 1;
      }
    }
    char parent[SNOVAC_PATH_MAX + 64];
    snprintf(parent, sizeof(parent), "%s/..", cur);
    normalize_path_into(parent, cur, sizeof(cur));
  }
  return 0;
}

void project_discover(const char *path, SnProject *out) {
  memset(out, 0, sizeof(*out));

  char start_dir[SNOVAC_PATH_MAX];
  if (path_is_dir(path)) {
    snprintf(start_dir, sizeof(start_dir), "%s", path);
  } else {
    dirname_into(path, start_dir, sizeof(start_dir));
  }

  char manifest_dir[SNOVAC_PATH_MAX];
  if (!find_manifest_dir(start_dir, manifest_dir, sizeof(manifest_dir))) {
    normalize_path_into(start_dir, out->source_root, sizeof(out->source_root));
    return;
  }
  out->has_manifest = 1;
  normalize_path_into(manifest_dir, out->manifest_dir, sizeof(out->manifest_dir));

  char src_dir[SNOVAC_PATH_MAX + 16];
  snprintf(src_dir, sizeof(src_dir), "%s/src", manifest_dir);
  if (path_is_dir(src_dir)) {
    normalize_path_into(src_dir, out->source_root, sizeof(out->source_root));
  } else {
    normalize_path_into(manifest_dir, out->source_root,
                        sizeof(out->source_root));
  }

  char deps[SNOVAC_PATH_MAX + 32];
  snprintf(deps, sizeof(deps), "%s/.snovalang/deps", manifest_dir);
  if (path_is_dir(deps)) {
    normalize_path_into(deps, out->deps_root, sizeof(out->deps_root));
  }

  char cache[SNOVAC_PATH_MAX + 32];
  snprintf(cache, sizeof(cache), "%s/.snovalang/cache", manifest_dir);
  if (path_is_dir(cache)) {
    normalize_path_into(cache, out->cache_root, sizeof(out->cache_root));
  }
}

void project_set_offline_cache(SnProject *proj, const char *cache_dir) {
  if (!proj)
    return;
  proj->use_offline_cache = 1;
  if (cache_dir && cache_dir[0]) {
    normalize_path_into(cache_dir, proj->cache_root, sizeof(proj->cache_root));
  } else if (!proj->cache_root[0]) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
      snprintf(proj->cache_root, sizeof(proj->cache_root),
               "%s/.snovalang/cache", home);
    } else {
      snprintf(proj->cache_root, sizeof(proj->cache_root), ".snovalang/cache");
    }
  }
}

int find_builtin_root_for_project(const char *source_root, char *out,
                                  size_t out_sz) {
  const char *env_dir = getenv("SNOVA_BUILTIN_DIR");
  if (env_dir && env_dir[0] && path_is_dir(env_dir)) {
    snprintf(out, out_sz, "%s", env_dir);
    return 1;
  }
  if (find_builtin_root(source_root, out, out_sz)) {
    return 1;
  }
  if (g_exe_dir[0] && find_builtin_root(g_exe_dir, out, out_sz)) {
    return 1;
  }
  return 0;
}

static void scan_manifest_deps(SnPackageGraph *graph, const SnProject *proj, const char *manifest_path) {
    FILE *f = fopen(manifest_path, "r");
    if (!f) {
        if (proj->deps_root[0]) sn_pkggraph_scan_root(graph, proj->deps_root);
        return;
    }
    char line[512];
    int in_deps = 0;
    int in_references = 0;
    int found_any = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strstr(p, "references = [") || strstr(p, "references=[") || strstr(p, "references [")) {
            in_references = 1;
        } else if (in_references && strchr(p, ']')) {
            in_references = 0;
        } else if (in_references) {
            char *q1 = strchr(p, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2) {
                    *q2 = '\0';
                    char sub_dir[SNOVAC_PATH_MAX];
                    snprintf(sub_dir, sizeof(sub_dir), "%s/%s", proj->manifest_dir, q1);
                    char sub_mod[SNOVAC_PATH_MAX];
                    snprintf(sub_mod, sizeof(sub_mod), "%s/mod.sno", sub_dir);
                    if (!path_is_dir(sub_dir)) {
                        fprintf(stderr, "error[SNOVA0052]: referenced submodule '%s' directory does not exist: %s\n", q1, sub_dir);
                        if (graph->diag) {
                            SnSpan zero = {0, 0, 0, 0};
                            sn_diag_emit(graph->diag, SN_DIAG_ERROR, 52, zero,
                                         "referenced submodule '%s' directory does not exist", q1);
                        }
                    } else {
                        /* Index submodule sources into graph */
                        sn_pkggraph_scan_root(graph, sub_dir);
                        if (path_is_file(sub_mod)) {
                            SnProject sub_proj;
                            memset(&sub_proj, 0, sizeof(sub_proj));
                            normalize_path_into(sub_dir, sub_proj.manifest_dir, sizeof(sub_proj.manifest_dir));
                            normalize_path_into(proj->deps_root, sub_proj.deps_root, sizeof(sub_proj.deps_root));
                            scan_manifest_deps(graph, &sub_proj, sub_mod);
                        }
                    }
                }
            }
        } else if (strstr(p, "direct = [") || strstr(p, "direct=[") || strstr(p, "indirect = [") || strstr(p, "indirect=[")) {
            in_deps = 1;
        } else if (in_deps && strchr(p, ']')) {
            in_deps = 0;
        } else if (in_deps) {
            char *q1 = strchr(p, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2) {
                    *q2 = '\0';
                    const char *arrow = strstr(q1, "->");
                    const char *mod = arrow ? arrow + 2 : q1;
                    while (*mod == ' ' || *mod == '\t') mod++;
                    char mod_buf[256];
                    snprintf(mod_buf, sizeof(mod_buf), "%s", mod);
                    char *at = strchr(mod_buf, '@');
                    if (at) *at = '\0';
                    size_t mlen = strlen(mod_buf);
                    while (mlen > 0 && (mod_buf[mlen - 1] == ' ' || mod_buf[mlen - 1] == '\t' || mod_buf[mlen - 1] == '\n' || mod_buf[mlen - 1] == '\r')) {
                        mod_buf[--mlen] = '\0';
                    }
                    const char *clean_mod = mod_buf;
                    if (strncmp(clean_mod, "https://", 8) == 0) clean_mod += 8;
                    if (strncmp(clean_mod, "http://", 7) == 0) clean_mod += 7;

                    if (clean_mod[0] && proj->deps_root[0]) {
                        char dep_dir[SNOVAC_PATH_MAX];
                        snprintf(dep_dir, sizeof(dep_dir), "%s/%s", proj->deps_root, clean_mod);
                        if (path_is_dir(dep_dir)) {
                            sn_pkggraph_scan_root(graph, dep_dir);
                            found_any = 1;
                        }
                    }
                }
            }
        }
    }
    fclose(f);
    if (!found_any && proj->deps_root[0]) {
        sn_pkggraph_scan_root(graph, proj->deps_root);
    }
}

size_t scan_project_roots(SnPackageGraph *graph, const SnProject *proj) {
  size_t own = sn_pkggraph_scan_root(graph, proj->source_root);
  if (proj->has_manifest) {
    char manifest_path[SNOVAC_PATH_MAX + 64];
    snprintf(manifest_path, sizeof(manifest_path), "%s/mod.sno", proj->manifest_dir);
    scan_manifest_deps(graph, proj, manifest_path);
  } else if (proj->deps_root[0]) {
    sn_pkggraph_scan_root(graph, proj->deps_root);
  }
  char builtin_dir[SNOVAC_PATH_MAX];
  if (find_builtin_root_for_project(proj->source_root, builtin_dir,
                                    sizeof(builtin_dir))) {
    sn_pkggraph_scan_root(graph, builtin_dir);
    sn_pkggraph_load_native_manifest(graph, builtin_dir);
  }
  return own;
}

SnList aggregate_imports(SnArena *a, SnPackageNode *node) {
  SnList out;
  memset(&out, 0, sizeof(out));
  for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
    for (size_t i = 0; i < pf->imports.len; i++) {
      const char *imp = SN_LIST_AT(pf->imports, const char, i);
      int dup = 0;
      for (size_t j = 0; j < out.len; j++) {
        if (SN_LIST_AT(out, const char, j) == imp) {
          dup = 1;
          break;
        }
      }
      if (!dup) {
        sn_list_push(a, &out, (void *)imp);
      }
    }
  }
  return out;
}
