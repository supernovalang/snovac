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

static const char *const MANIFEST_NAMES[] = {"snova.toml", "builtin.toml.Toml"};

static int find_manifest_dir(const char *start_dir, char *out, size_t out_sz) {
    char cur[SNOVAC_PATH_MAX + 64];
    snprintf(cur, sizeof(cur), "%s", start_dir);

    for (int depth = 0; depth < 32; depth++) {
        for (size_t i = 0; i < sizeof(MANIFEST_NAMES) / sizeof(MANIFEST_NAMES[0]); i++) {
            char candidate[SNOVAC_PATH_MAX + 128];
            snprintf(candidate, sizeof(candidate), "%s/%s", cur, MANIFEST_NAMES[i]);
            if (path_is_file(candidate)) {
                snprintf(out, out_sz, "%s", cur);
                return 1;
            }
        }
        char parent[SNOVAC_PATH_MAX + 64];
        snprintf(parent, sizeof(parent), "%s/..", cur);
        snprintf(cur, sizeof(cur), "%s", parent);
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

    char src_dir[SNOVAC_PATH_MAX + 16];
    snprintf(src_dir, sizeof(src_dir), "%s/src", manifest_dir);
    if (path_is_dir(src_dir)) {
        normalize_path_into(src_dir, out->source_root, sizeof(out->source_root));
    } else {
        normalize_path_into(manifest_dir, out->source_root, sizeof(out->source_root));
    }

    char deps[SNOVAC_PATH_MAX + 32];
    snprintf(deps, sizeof(deps), "%s/.snovalang/deps", manifest_dir);
    if (path_is_dir(deps)) {
        normalize_path_into(deps, out->deps_root, sizeof(out->deps_root));
    }
}

int find_builtin_root_for_project(const char *source_root, char *out, size_t out_sz) {
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

size_t scan_project_roots(SnPackageGraph *graph, const SnProject *proj) {
    size_t own = sn_pkggraph_scan_root(graph, proj->source_root);
    if (proj->deps_root[0]) {
        sn_pkggraph_scan_root(graph, proj->deps_root);
    }
    char builtin_dir[SNOVAC_PATH_MAX];
    if (find_builtin_root_for_project(proj->source_root, builtin_dir, sizeof(builtin_dir))) {
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
