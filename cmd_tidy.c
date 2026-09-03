/* cmd_tidy.c — automated dependency pruning and snova.mdlo manifest generator. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_tidy.h"
#include "arena.h"
#include "diag.h"
#include "driver_utils.h"
#include "intern.h"
#include "lex.h"
#include "package.h"
#include "parse.h"
#include "project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *pkg_prefix;
    const char *module_id;
    const char *version;
} KnownModule;

static const KnownModule KNOWN_MODULES[] = {
    {"Snova.Std.Network.Http",   "github.com/supernovalang/snova-http",          "1.0.0"},
    {"Snova.Std.Network.Tcp",    "github.com/supernovalang/snova-http",          "1.0.0"},
    {"Snova.Std.Network",        "github.com/supernovalang/snova-http",          "1.0.0"},
    {"Snova.Std.Http",          "github.com/supernovalang/snova-http",          "1.0.0"},
    {"Snova.Std.Server",        "github.com/supernovalang/snova-http",          "1.0.0"},
    {"Snova.Std.Collections",   "github.com/supernovalang/snova-collections",   "1.0.0"},
    {"Snova.Std.Serialization", "github.com/supernovalang/snova-serialization", "1.0.0"},
    {"Snova.Std.Strings",       "github.com/supernovalang/snova-strings",       "1.0.0"},
    {"Snova.Std.Async",         "github.com/supernovalang/snova-async",         "1.0.0"},
    {"Snova.Std.IO",            "github.com/supernovalang/snova-io",            "1.0.0"},
    {"Snova.Std",               "github.com/supernovalang/snova-std",           "1.0.0"},
};

typedef struct {
    const char *from_module;
    const char *to_module;
} KnownIndirectEdge;

static const KnownIndirectEdge KNOWN_INDIRECT_EDGES[] = {
    {"github.com/supernovalang/snova-std", "github.com/supernovalang/snova-collections"},
    {"github.com/supernovalang/snova-std", "github.com/supernovalang/snova-strings"},
    {"github.com/supernovalang/snova-std", "github.com/supernovalang/snova-io"},
    {"github.com/supernovalang/snova-std", "github.com/supernovalang/snova-serialization"},
    {"github.com/supernovalang/snova-std", "github.com/supernovalang/snova-async"},
    {"github.com/supernovalang/snova-std", "github.com/supernovalang/snova-http"},
    {"github.com/supernovalang/snova-http", "github.com/supernovalang/snova-collections"},
    {"github.com/supernovalang/snova-http", "github.com/supernovalang/snova-strings"},
    {"github.com/supernovalang/snova-http", "github.com/supernovalang/snova-io"},
};

static void read_module_header(const char *manifest_path, char *mod_name, size_t mod_sz, char *snova_ver, size_t ver_sz) {
    snprintf(mod_name, mod_sz, "app");
    snprintf(snova_ver, ver_sz, "1.0.0");

    FILE *f = fopen(manifest_path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "module ", 7) == 0) {
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
                p[--len] = '\0';
            }
            if (len > 0) snprintf(mod_name, mod_sz, "%s", p);
        } else if (strncmp(p, "snova ", 6) == 0) {
            p += 6;
            while (*p == ' ' || *p == '\t' || *p == '"') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '"')) {
                p[--len] = '\0';
            }
            if (len > 0) snprintf(snova_ver, ver_sz, "%s", p);
        }
    }
    fclose(f);
}

int cmd_tidy_project(const char *path) {
    SnProject proj;
    project_discover(path, &proj);

    char manifest_path[SNOVAC_PATH_MAX + 64];
    char proj_root[SNOVAC_PATH_MAX];
    if (proj.has_manifest) {
        dirname_into(proj.source_root, proj_root, sizeof(proj_root));
        snprintf(manifest_path, sizeof(manifest_path), "%s/snova.mdlo", proj_root);
    } else {
        snprintf(proj_root, sizeof(proj_root), "%s", path);
        snprintf(manifest_path, sizeof(manifest_path), "%s/snova.mdlo", path);
    }

    char mod_name[256];
    char snova_ver[64];
    read_module_header(manifest_path, mod_name, sizeof(mod_name), snova_ver, sizeof(snova_ver));

    SnArena arena;
    sn_arena_init(&arena, 4 * 1024 * 1024);
    SnInternTable intern;
    sn_intern_init(&intern, &arena);
    SnDiagSink diag;
    sn_diag_init(&diag, path, "", 0);

    SnPackageGraph graph;
    sn_pkggraph_init(&graph, &arena, &intern, &diag);
    sn_pkggraph_scan_root(&graph, proj.source_root);

    /* Collect all imported package names in the project */
    const char *used_modules[32];
    size_t used_mod_count = 0;

    for (SnPackageNode *node = graph.nodes; node; node = node->next) {
        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            SnDiagFile self = {pf->path, pf->src, pf->src_len};
            SnDiagFile outer = sn_diag_set_file(&diag, self);

            SnTokenVec toks;
            sn_lex(&arena, &diag, pf->src, pf->src_len, &toks);

            SnUnit unit;
            sn_parse(&arena, &diag, &toks, &unit);

            for (size_t ii = 0; ii < unit.imports.len; ii++) {
                const char *imp = SN_LIST_AT(unit.imports, const char, ii);
                if (!imp) continue;
                /* Match against known modules */
                for (size_t k = 0; k < sizeof(KNOWN_MODULES)/sizeof(KNOWN_MODULES[0]); k++) {
                    if (strncmp(imp, KNOWN_MODULES[k].pkg_prefix, strlen(KNOWN_MODULES[k].pkg_prefix)) == 0) {
                        int already = 0;
                        for (size_t u = 0; u < used_mod_count; u++) {
                            if (strcmp(used_modules[u], KNOWN_MODULES[k].module_id) == 0) {
                                already = 1;
                                break;
                            }
                        }
                        if (!already && used_mod_count < sizeof(used_modules)/sizeof(used_modules[0])) {
                            used_modules[used_mod_count++] = KNOWN_MODULES[k].module_id;
                        }
                    }
                }
            }
            sn_diag_set_file(&diag, outer);
        }
    }

    /* Collect active indirect edges based on used modules */
    const char *indirect_edges[64];
    size_t indirect_count = 0;

    for (size_t e = 0; e < sizeof(KNOWN_INDIRECT_EDGES)/sizeof(KNOWN_INDIRECT_EDGES[0]); e++) {
        const char *from = KNOWN_INDIRECT_EDGES[e].from_module;
        const char *to = KNOWN_INDIRECT_EDGES[e].to_module;

        int from_used = 0;
        int to_used = 0;
        for (size_t u = 0; u < used_mod_count; u++) {
            if (strcmp(used_modules[u], from) == 0) from_used = 1;
            if (strcmp(used_modules[u], to) == 0) to_used = 1;
        }

        if (from_used && to_used) {
            char edge_buf[512];
            snprintf(edge_buf, sizeof(edge_buf), "%s -> %s", from, to);
            indirect_edges[indirect_count++] = sn_intern_cstr(&intern, edge_buf);
        }
    }

    /* Write updated snova.mdlo */
    FILE *out = fopen(manifest_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write manifest to %s\n", manifest_path);
        sn_arena_free(&arena);
        return 1;
    }

    fprintf(out, "module %s\n\n", mod_name);
    fprintf(out, "snova \"%s\"\n\n", snova_ver);
    fprintf(out, "dependencies(\n");
    fprintf(out, "    direct = [\n");
    for (size_t u = 0; u < used_mod_count; u++) {
        fprintf(out, "        \"%s@1.0.0\"%s\n", used_modules[u], (u + 1 < used_mod_count) ? "," : "");
    }
    fprintf(out, "    ],\n");
    fprintf(out, "    indirect = [\n");
    for (size_t i = 0; i < indirect_count; i++) {
        fprintf(out, "        \"%s\"%s\n", indirect_edges[i], (i + 1 < indirect_count) ? "," : "");
    }
    fprintf(out, "    ]\n");
    fprintf(out, ")\n");
    fclose(out);

    printf("snovac tidy: updated %s (%zu direct, %zu indirect dependencies)\n",
           manifest_path, used_mod_count, indirect_count);

    sn_arena_free(&arena);
    return 0;
}
