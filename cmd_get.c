#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "cmd_get.h"
#include "driver_utils.h"
#include "project.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ── String helpers ──────────────────────────────────────────────────────── */

static void str_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* ── File and Directory Copying (No Shell) ───────────────────────────────── */

static int copy_file_direct(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }
    fclose(in);
    fclose(out);
    return 1;
}

static int copy_dir_recursive(const char *src_dir, const char *dst_dir) {
    DIR *d = opendir(src_dir);
    if (!d) return 0;

    ensure_parent_dir_exists(dst_dir);
#if defined(_WIN32)
    mkdir(dst_dir);
#else
    mkdir(dst_dir, 0755);
#endif

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, ".git") == 0) continue;

        char src_path[SNOVAC_PATH_MAX];
        char dst_path[SNOVAC_PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, ent->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, ent->d_name);

        struct stat st;
        if (stat(src_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                copy_dir_recursive(src_path, dst_path);
            } else if (S_ISREG(st.st_mode)) {
                copy_file_direct(src_path, dst_path);
            }
        }
    }
    closedir(d);
    return 1;
}

/* ── Safe Git Clone (No Shell Interpolation) ──────────────────────────────── */

static int run_git_clone_safe(const char *url, const char *version, const char *dest_dir) {
#if defined(_WIN32)
    char *argv[10];
    int argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "clone";
    if (version && version[0]) {
        argv[argc++] = "--branch";
        argv[argc++] = (char *)version;
    }
    argv[argc++] = "--depth";
    argv[argc++] = "1";
    argv[argc++] = (char *)url;
    argv[argc++] = (char *)dest_dir;
    argv[argc] = NULL;

    intptr_t status = _spawnvp(_P_WAIT, "git", argv);
    return (status == 0) ? 0 : 1;
#else
    char *argv[10];
    int argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "clone";
    if (version && version[0]) {
        argv[argc++] = "--branch";
        argv[argc++] = (char *)version;
    }
    argv[argc++] = "--depth";
    argv[argc++] = "1";
    argv[argc++] = (char *)url;
    argv[argc++] = (char *)dest_dir;
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp("git", argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

static int fetch_dependency(const char *url_or_path, const char *version, const char *target_dir) {
    if (path_is_dir(target_dir)) {
        return 0; /* Already fetched */
    }

    ensure_parent_dir_exists(target_dir);

    /* Check if local filesystem path */
    if (strncmp(url_or_path, "file://", 7) == 0 || path_is_dir(url_or_path) ||
        url_or_path[0] == '/' || url_or_path[0] == '.') {
        const char *local_src = (strncmp(url_or_path, "file://", 7) == 0) ? url_or_path + 7 : url_or_path;
        if (path_is_dir(local_src)) {
            if (version && version[0]) {
                char v_tag[128];
                snprintf(v_tag, sizeof(v_tag), "v%s", (version[0] == 'v' || version[0] == 'V') ? version + 1 : version);
                if (run_git_clone_safe(local_src, v_tag, target_dir) == 0) return 0;
                if (run_git_clone_safe(local_src, version, target_dir) == 0) return 0;
            }
            if (run_git_clone_safe(local_src, NULL, target_dir) == 0) return 0;
            if (copy_dir_recursive(local_src, target_dir)) return 0;
        }
    }

    /* Remote git repo */
    char full_url[1024];
    if (strncmp(url_or_path, "http://", 7) != 0 &&
        strncmp(url_or_path, "https://", 8) != 0 &&
        strncmp(url_or_path, "git@", 4) != 0 &&
        strncmp(url_or_path, "ssh://", 6) != 0) {
        snprintf(full_url, sizeof(full_url), "https://%s", url_or_path);
    } else {
        snprintf(full_url, sizeof(full_url), "%s", url_or_path);
    }

    /* 1. Try release tag `v<version>` (e.g. `v1.0.0`) */
    if (version && version[0]) {
        char v_tag[128];
        if (version[0] == 'v' || version[0] == 'V') {
            snprintf(v_tag, sizeof(v_tag), "%s", version);
        } else {
            snprintf(v_tag, sizeof(v_tag), "v%s", version);
        }
        if (run_git_clone_safe(full_url, v_tag, target_dir) == 0) {
            return 0;
        }

        /* 2. Try release tag `<version>` (e.g. `1.0.0`) */
        const char *raw_ver = (version[0] == 'v' || version[0] == 'V') ? version + 1 : version;
        if (run_git_clone_safe(full_url, raw_ver, target_dir) == 0) {
            return 0;
        }
    }

    /* 3. Fallback: clone default branch / HEAD */
    int rc = run_git_clone_safe(full_url, NULL, target_dir);
    return rc;
}

/* ── Manifest Representation & Parser ────────────────────────────────────── */

typedef struct SnManifestDep {
    char raw[512];
    char from[256];
    char module[256];
    char version[64];
    int is_edge;
    struct SnManifestDep *next;
} SnManifestDep;

typedef struct SnManifest {
    char module_name[256];
    char module_url[512];
    char snova_version[64];
    SnManifestDep *direct;
    SnManifestDep *indirect;
    int direct_count;
    int indirect_count;
} SnManifest;

static void parse_dep_string(const char *str, SnManifestDep *dep) {
    memset(dep, 0, sizeof(*dep));
    snprintf(dep->raw, sizeof(dep->raw), "%s", str);
    str_trim(dep->raw);

    const char *arrow = strstr(str, "->");
    if (arrow) {
        dep->is_edge = 1;
        size_t from_len = (size_t)(arrow - str);
        char from_buf[256];
        if (from_len >= sizeof(from_buf)) from_len = sizeof(from_buf) - 1;
        memcpy(from_buf, str, from_len);
        from_buf[from_len] = '\0';
        str_trim(from_buf);
        snprintf(dep->from, sizeof(dep->from), "%s", from_buf);

        const char *to_str = arrow + 2;
        char to_buf[256];
        snprintf(to_buf, sizeof(to_buf), "%s", to_str);
        str_trim(to_buf);
        char *at = strchr(to_buf, '@');
        if (at) {
            *at = '\0';
            snprintf(dep->version, sizeof(dep->version), "%s", at + 1);
            str_trim(dep->version);
        }
        str_trim(to_buf);
        snprintf(dep->module, sizeof(dep->module), "%s", to_buf);
    } else {
        dep->is_edge = 0;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", str);
        str_trim(buf);
        char *at = strchr(buf, '@');
        if (at) {
            *at = '\0';
            snprintf(dep->version, sizeof(dep->version), "%s", at + 1);
            str_trim(dep->version);
        }
        str_trim(buf);
        snprintf(dep->module, sizeof(dep->module), "%s", buf);
    }
}

static int find_dir_manifest(const char *dir, char *out_path, size_t out_sz) {
    const char *names[] = {"mod.sno", "snova.mod", "snova.sno", "snova.toml"};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        char cand[SNOVAC_PATH_MAX + 32];
        snprintf(cand, sizeof(cand), "%s/%s", dir, names[i]);
        if (path_is_file(cand)) {
            snprintf(out_path, out_sz, "%s", cand);
            return 1;
        }
    }
    return 0;
}

static int manifest_read(const char *manifest_path, SnManifest *m) {
    memset(m, 0, sizeof(*m));
    snprintf(m->module_name, sizeof(m->module_name), "app");
    snprintf(m->snova_version, sizeof(m->snova_version), "1.0.0");

    FILE *f = fopen(manifest_path, "r");
    if (!f) return 0;

    char line[1024];
    int in_direct = 0;
    int in_indirect = 0;
    int in_multiline_comment = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        str_trim(p);

        if (in_multiline_comment) {
            char *end_c = strstr(p, "*/");
            if (end_c) {
                in_multiline_comment = 0;
                p = end_c + 2;
                str_trim(p);
            } else {
                continue;
            }
        }
        char *start_c = strstr(p, "/*");
        if (start_c) {
            char *end_c = strstr(start_c + 2, "*/");
            if (end_c) {
                memmove(start_c, end_c + 2, strlen(end_c + 2) + 1);
                str_trim(p);
            } else {
                *start_c = '\0';
                in_multiline_comment = 1;
                str_trim(p);
            }
        }

        char *comment = strstr(p, "//");
        if (comment) *comment = '\0';
        if (p[0] == '#') *p = '\0';
        str_trim(p);
        if (p[0] == '\0') continue;

        if (strncmp(p, "module", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            str_trim(p);
            char *sp = strpbrk(p, " \t");
            if (sp) {
                *sp = '\0';
                snprintf(m->module_name, sizeof(m->module_name), "%s", p);
                p = sp + 1;
                str_trim(p);
                if (p[0]) snprintf(m->module_url, sizeof(m->module_url), "%s", p);
            } else {
                snprintf(m->module_name, sizeof(m->module_name), "%s", p);
            }
            continue;
        }

        if (strncmp(p, "snova", 5) == 0 && (p[5] == ' ' || p[5] == '\t' || p[5] == '"')) {
            p += 5;
            str_trim(p);
            if (p[0] == '"') {
                p++;
                char *q = strchr(p, '"');
                if (q) *q = '\0';
            }
            snprintf(m->snova_version, sizeof(m->snova_version), "%s", p);
            continue;
        }

        if (strstr(p, "indirect") && strchr(p, '=')) {
            in_indirect = 1;
            in_direct = 0;
        } else if (strstr(p, "direct") && strchr(p, '=')) {
            in_direct = 1;
            in_indirect = 0;
        } else if (strstr(p, "require") && strchr(p, '(')) {
            in_direct = 1;
            in_indirect = 0;
            continue;
        }

        /* Go-style require ( github.com/user/pkg v1.0.0 ) without quotes */
        if (in_direct && !strchr(p, '"') && !strchr(p, '[') && !strchr(p, ']') && !strchr(p, '(') && !strchr(p, ')')) {
            char mod_id[256] = {0};
            char mod_ver[64] = {0};
            if (sscanf(p, "%255s %63s", mod_id, mod_ver) >= 1) {
                if (mod_id[0] && mod_id[0] != '/' && mod_id[0] != '#' && strchr(mod_id, '.')) {
                    SnManifestDep *dep = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
                    snprintf(dep->module, sizeof(dep->module), "%s", mod_id);
                    if (mod_ver[0]) snprintf(dep->version, sizeof(dep->version), "%s", mod_ver);
                    else snprintf(dep->version, sizeof(dep->version), "1.0.0");
                    snprintf(dep->raw, sizeof(dep->raw), "%s@%s", dep->module, dep->version);
                    dep->next = NULL;
                    if (!m->direct) m->direct = dep;
                    else {
                        SnManifestDep *cur = m->direct;
                        while (cur->next) cur = cur->next;
                        cur->next = dep;
                    }
                    m->direct_count++;
                    continue;
                }
            }
        }

        char *quote_start = p;
        while ((quote_start = strchr(quote_start, '"')) != NULL) {
            quote_start++;
            char *quote_end = strchr(quote_start, '"');
            if (!quote_end) break;
            *quote_end = '\0';

            SnManifestDep *dep = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
            parse_dep_string(quote_start, dep);

            if (in_direct) {
                dep->next = NULL;
                if (!m->direct) {
                    m->direct = dep;
                } else {
                    SnManifestDep *cur = m->direct;
                    while (cur->next) cur = cur->next;
                    cur->next = dep;
                }
                m->direct_count++;
            } else if (in_indirect) {
                dep->next = NULL;
                if (!m->indirect) {
                    m->indirect = dep;
                } else {
                    SnManifestDep *cur = m->indirect;
                    while (cur->next) cur = cur->next;
                    cur->next = dep;
                }
                m->indirect_count++;
            } else {
                free(dep);
            }

            quote_start = quote_end + 1;
        }

        if (strchr(p, ']') || (in_direct && strchr(p, ')'))) {
            if (in_direct) in_direct = 0;
            else if (in_indirect) in_indirect = 0;
        }
    }

    fclose(f);
    return 1;
}

static void manifest_free(SnManifest *m) {
    SnManifestDep *d = m->direct;
    while (d) {
        SnManifestDep *next = d->next;
        free(d);
        d = next;
    }
    m->direct = NULL;
    m->direct_count = 0;

    SnManifestDep *ind = m->indirect;
    while (ind) {
        SnManifestDep *next = ind->next;
        free(ind);
        ind = next;
    }
    m->indirect = NULL;
    m->indirect_count = 0;
}

static int compare_manifest_deps(const void *a, const void *b) {
    const SnManifestDep *const *da = (const SnManifestDep *const *)a;
    const SnManifestDep *const *db = (const SnManifestDep *const *)b;
    return strcmp((*da)->raw, (*db)->raw);
}

static int manifest_write(const char *manifest_path, const SnManifest *m) {
    FILE *out = fopen(manifest_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write manifest to %s\n", manifest_path);
        return 0;
    }

    if (m->module_url[0]) {
        fprintf(out, "module %s %s\n\n", m->module_name[0] ? m->module_name : "app", m->module_url);
    } else {
        fprintf(out, "module %s\n\n", m->module_name[0] ? m->module_name : "app");
    }

    fprintf(out, "snova \"%s\"\n\n", m->snova_version[0] ? m->snova_version : "1.0.0");

    SnManifestDep **direct_arr = NULL;
    if (m->direct_count > 0) {
        direct_arr = (SnManifestDep **)malloc(sizeof(SnManifestDep *) * (size_t)m->direct_count);
        int idx = 0;
        for (SnManifestDep *d = m->direct; d && idx < m->direct_count; d = d->next) {
            direct_arr[idx++] = d;
        }
        qsort(direct_arr, (size_t)m->direct_count, sizeof(SnManifestDep *), compare_manifest_deps);
    }

    SnManifestDep **indirect_arr = NULL;
    if (m->indirect_count > 0) {
        indirect_arr = (SnManifestDep **)malloc(sizeof(SnManifestDep *) * (size_t)m->indirect_count);
        int idx = 0;
        for (SnManifestDep *d = m->indirect; d && idx < m->indirect_count; d = d->next) {
            indirect_arr[idx++] = d;
        }
        qsort(indirect_arr, (size_t)m->indirect_count, sizeof(SnManifestDep *), compare_manifest_deps);
    }

    fprintf(out, "dependencies(\n");
    fprintf(out, "    direct = [\n");
    for (int i = 0; i < m->direct_count; i++) {
        fprintf(out, "        \"%s\"%s\n", direct_arr[i]->raw, (i + 1 < m->direct_count) ? "," : "");
    }
    fprintf(out, "    ],\n");
    fprintf(out, "    indirect = [\n");
    for (int i = 0; i < m->indirect_count; i++) {
        fprintf(out, "        \"%s\"%s\n", indirect_arr[i]->raw, (i + 1 < m->indirect_count) ? "," : "");
    }
    fprintf(out, "    ]\n");
    fprintf(out, ")\n");

    if (direct_arr) free(direct_arr);
    if (indirect_arr) free(indirect_arr);
    fclose(out);
    return 1;
}

/* ── Dependency Identification & Normalization ───────────────────────────── */

static void normalize_module_info(const char *input, const char *version_override,
                                  char *out_id, size_t out_id_sz,
                                  char *out_ver, size_t out_ver_sz,
                                  char *out_url, size_t out_url_sz,
                                  char *out_rel_dir, size_t out_rel_dir_sz) {
    char raw[1024];
    snprintf(raw, sizeof(raw), "%s", input);
    str_trim(raw);

    char ver[64] = {0};
    char *at = strchr(raw, '@');
    if (at) {
        *at = '\0';
        snprintf(ver, sizeof(ver), "%s", at + 1);
        str_trim(ver);
    }

    if (version_override && version_override[0]) {
        snprintf(ver, sizeof(ver), "%s", version_override);
        str_trim(ver);
    }
    if (ver[0] == '\0') {
        snprintf(ver, sizeof(ver), "1.0.0");
    }
    snprintf(out_ver, out_ver_sz, "%s", ver);

    /* Check local path */
    if (strncmp(raw, "file://", 7) == 0 || path_is_dir(raw) || raw[0] == '/' || raw[0] == '.') {
        const char *local_path = (strncmp(raw, "file://", 7) == 0) ? raw + 7 : raw;
        snprintf(out_url, out_url_sz, "%s", local_path);

        /* Inspect mod.sno inside local dir for canonical module name/url if available */
        char mod_path[SNOVAC_PATH_MAX + 32];
        snprintf(mod_path, sizeof(mod_path), "%s/mod.sno", local_path);
        SnManifest local_m;
        if (path_is_file(mod_path) && manifest_read(mod_path, &local_m)) {
            if (local_m.module_url[0]) {
                snprintf(out_id, out_id_sz, "%s", local_m.module_url);
            } else if (local_m.module_name[0]) {
                snprintf(out_id, out_id_sz, "%s", local_m.module_name);
            } else {
                const char *slash = strrchr(local_path, '/');
                snprintf(out_id, out_id_sz, "%s", slash ? slash + 1 : local_path);
            }
            manifest_free(&local_m);
        } else {
            const char *slash = strrchr(local_path, '/');
            snprintf(out_id, out_id_sz, "%s", slash ? slash + 1 : local_path);
        }

        const char *slash = strrchr(local_path, '/');
        snprintf(out_rel_dir, out_rel_dir_sz, "%s", slash ? slash + 1 : local_path);
        return;
    }

    /* URL normalization */
    char clean_id[512];
    snprintf(clean_id, sizeof(clean_id), "%s", raw);

    if (strncmp(clean_id, "https://", 8) == 0) {
        memmove(clean_id, clean_id + 8, strlen(clean_id + 8) + 1);
    } else if (strncmp(clean_id, "http://", 7) == 0) {
        memmove(clean_id, clean_id + 7, strlen(clean_id + 7) + 1);
    } else if (strncmp(clean_id, "git@", 4) == 0) {
        memmove(clean_id, clean_id + 4, strlen(clean_id + 4) + 1);
        char *colon = strchr(clean_id, ':');
        if (colon) *colon = '/';
    }

    size_t len = strlen(clean_id);
    if (len > 4 && strcmp(clean_id + len - 4, ".git") == 0) {
        clean_id[len - 4] = '\0';
        len -= 4;
    }
    while (len > 0 && clean_id[len - 1] == '/') {
        clean_id[--len] = '\0';
    }

    snprintf(out_id, out_id_sz, "%s", clean_id);
    snprintf(out_rel_dir, out_rel_dir_sz, "%s", clean_id);

    if (strncmp(raw, "http://", 7) == 0 || strncmp(raw, "https://", 8) == 0 ||
        strncmp(raw, "git@", 4) == 0 || strncmp(raw, "ssh://", 6) == 0) {
        snprintf(out_url, out_url_sz, "%s", raw);
    } else {
        snprintf(out_url, out_url_sz, "https://%s", clean_id);
    }
}

/* ── Dependency Graph Resolver & Cycle Detection ────────────────────────── */

typedef enum {
    DEP_STATE_UNVISITED = 0,
    DEP_STATE_VISITING,
    DEP_STATE_VISITED
} DepVisitState;

typedef struct DepNode {
    char id[256];
    char url[512];
    char version[64];
    char local_dir[SNOVAC_PATH_MAX];
    int is_direct;
    int is_root;
    DepVisitState state;
    struct DepNode *next;
} DepNode;

typedef struct DepEdge {
    char from[256];
    char to[256];
    struct DepEdge *next;
} DepEdge;

typedef struct {
    char proj_root[SNOVAC_PATH_MAX];
    char deps_root[SNOVAC_PATH_MAX];
    DepNode *nodes;
    DepEdge *edges;
    DepNode *stack[128];
    int stack_depth;
    int has_cycle;
    char cycle_msg[512];
} DepGraph;

static DepNode *graph_find_node(DepGraph *g, const char *id) {
    for (DepNode *n = g->nodes; n; n = n->next) {
        if (strcmp(n->id, id) == 0) return n;
    }
    return NULL;
}

static DepNode *graph_add_node(DepGraph *g, const char *id, const char *url,
                               const char *version, const char *local_dir,
                               int is_direct, int is_root) {
    DepNode *n = graph_find_node(g, id);
    if (n) {
        if (is_direct) n->is_direct = 1;
        if (version && version[0] && (!n->version[0] || strcmp(n->version, "1.0.0") == 0)) {
            snprintf(n->version, sizeof(n->version), "%s", version);
        }
        return n;
    }
    n = (DepNode *)calloc(1, sizeof(DepNode));
    snprintf(n->id, sizeof(n->id), "%s", id);
    snprintf(n->url, sizeof(n->url), "%s", url ? url : "");
    snprintf(n->version, sizeof(n->version), "%s", version ? version : "1.0.0");
    snprintf(n->local_dir, sizeof(n->local_dir), "%s", local_dir ? local_dir : "");
    n->is_direct = is_direct;
    n->is_root = is_root;
    n->state = DEP_STATE_UNVISITED;
    n->next = g->nodes;
    g->nodes = n;
    return n;
}

static void graph_add_edge(DepGraph *g, const char *from, const char *to) {
    for (DepEdge *e = g->edges; e; e = e->next) {
        if (strcmp(e->from, from) == 0 && strcmp(e->to, to) == 0) return;
    }
    DepEdge *e = (DepEdge *)calloc(1, sizeof(DepEdge));
    snprintf(e->from, sizeof(e->from), "%s", from);
    snprintf(e->to, sizeof(e->to), "%s", to);
    e->next = g->edges;
    g->edges = e;
}

static int graph_resolve_node(DepGraph *g, DepNode *node) {
    /* Cycle detection */
    for (int i = 0; i < g->stack_depth; i++) {
        if (g->stack[i] == node) {
            g->has_cycle = 1;
            snprintf(g->cycle_msg, sizeof(g->cycle_msg), "dependency cycle detected: ");
            for (int j = i; j < g->stack_depth; j++) {
                strncat(g->cycle_msg, g->stack[j]->id, sizeof(g->cycle_msg) - strlen(g->cycle_msg) - 1);
                strncat(g->cycle_msg, " -> ", sizeof(g->cycle_msg) - strlen(g->cycle_msg) - 1);
            }
            strncat(g->cycle_msg, node->id, sizeof(g->cycle_msg) - strlen(g->cycle_msg) - 1);
            return 1;
        }
    }

    if (node->state == DEP_STATE_VISITED) {
        return 0;
    }

    if (g->stack_depth >= 120) {
        fprintf(stderr, "error: dependency graph too deep\n");
        return 1;
    }

    g->stack[g->stack_depth++] = node;
    node->state = DEP_STATE_VISITING;

    if (node->is_root) {
        for (DepNode *n = g->nodes; n; n = n->next) {
            if (n->is_direct && !n->is_root) {
                int rc = graph_resolve_node(g, n);
                if (rc != 0) {
                    g->stack_depth--;
                    return rc;
                }
            }
        }
    } else {
        if (!path_is_dir(node->local_dir)) {
            printf("snovac get: fetching %s...\n", node->url);
            int rc = fetch_dependency(node->url, node->version, node->local_dir);
            if (rc != 0) {
                fprintf(stderr, "error: failed to fetch dependency from %s\n", node->url);
                g->stack_depth--;
                return 1;
            }
            printf("snovac get: fetched %s successfully\n", node->id);
        }

        char mod_path[SNOVAC_PATH_MAX + 32];
        SnManifest manifest;
        if (find_dir_manifest(node->local_dir, mod_path, sizeof(mod_path)) && manifest_read(mod_path, &manifest)) {
            for (SnManifestDep *d = manifest.direct; d; d = d->next) {
                char d_id[256];
                char d_ver[64];
                char d_url[512];
                char d_rel[SNOVAC_PATH_MAX];
                normalize_module_info(d->raw, d->version, d_id, sizeof(d_id), d_ver, sizeof(d_ver),
                                      d_url, sizeof(d_url), d_rel, sizeof(d_rel));

                char d_local[SNOVAC_PATH_MAX];
                snprintf(d_local, sizeof(d_local), "%s/%s", g->deps_root, d_rel);

                graph_add_edge(g, node->id, d_id);

                DepNode *d_node = graph_add_node(g, d_id, d_url, d_ver, d_local, 0, 0);
                int rc = graph_resolve_node(g, d_node);
                if (rc != 0) {
                    manifest_free(&manifest);
                    g->stack_depth--;
                    return rc;
                }
            }
            manifest_free(&manifest);
        }
    }

    g->stack_depth--;
    node->state = DEP_STATE_VISITED;
    return 0;
}

static void graph_free(DepGraph *g) {
    DepNode *n = g->nodes;
    while (n) {
        DepNode *next = n->next;
        free(n);
        n = next;
    }
    g->nodes = NULL;

    DepEdge *e = g->edges;
    while (e) {
        DepEdge *next = e->next;
        free(e);
        e = next;
    }
    g->edges = NULL;
}

/* ── CLI Command Implementation ──────────────────────────────────────────── */

int cmd_get_project(const char *proj_path, const char *url, const char *version) {
    SnProject proj;
    project_discover(proj_path ? proj_path : ".", &proj);

    char proj_root[SNOVAC_PATH_MAX];
    char manifest_path[SNOVAC_PATH_MAX + 32];
    char deps_root[SNOVAC_PATH_MAX];

    if (proj.has_manifest) {
        dirname_into(proj.source_root, proj_root, sizeof(proj_root));
    } else {
        snprintf(proj_root, sizeof(proj_root), "%s", proj_path ? proj_path : ".");
    }

    if (!find_dir_manifest(proj_root, manifest_path, sizeof(manifest_path))) {
        snprintf(manifest_path, sizeof(manifest_path), "%s/mod.sno", proj_root);
    }

    snprintf(deps_root, sizeof(deps_root), "%s/.snovalang/deps", proj_root);

    if (!path_is_dir(deps_root)) {
        ensure_parent_dir_exists(deps_root);
#if defined(_WIN32)
        mkdir(deps_root);
#else
        mkdir(deps_root, 0755);
#endif
    }

    SnManifest root_manifest;
    if (path_is_file(manifest_path)) {
        if (!manifest_read(manifest_path, &root_manifest)) {
            fprintf(stderr, "error: failed to read manifest from %s\n", manifest_path);
            return 1;
        }
    } else {
        memset(&root_manifest, 0, sizeof(root_manifest));
        const char *base = strrchr(proj_root, '/');
        snprintf(root_manifest.module_name, sizeof(root_manifest.module_name), "%s", base ? base + 1 : "app");
        snprintf(root_manifest.snova_version, sizeof(root_manifest.snova_version), "1.0.0");
    }

    if (!url && root_manifest.direct_count == 0) {
        fprintf(stderr, "error: get requires a repository url or declared dependencies in mod.sno\n");
        manifest_free(&root_manifest);
        return 2;
    }

    DepGraph graph;
    memset(&graph, 0, sizeof(graph));
    snprintf(graph.proj_root, sizeof(graph.proj_root), "%s", proj_root);
    snprintf(graph.deps_root, sizeof(graph.deps_root), "%s", deps_root);

    /* Add root node */
    DepNode *root_node = graph_add_node(&graph, root_manifest.module_name, NULL, root_manifest.snova_version, proj_root, 0, 1);

    /* If URL is supplied, add/update as root direct dependency */
    if (url && url[0]) {
        char req_id[256];
        char req_ver[64];
        char req_url[512];
        char req_rel[SNOVAC_PATH_MAX];
        normalize_module_info(url, version, req_id, sizeof(req_id), req_ver, sizeof(req_ver),
                              req_url, sizeof(req_url), req_rel, sizeof(req_rel));

        char req_local[SNOVAC_PATH_MAX];
        snprintf(req_local, sizeof(req_local), "%s/%s", deps_root, req_rel);

        graph_add_node(&graph, req_id, req_url, req_ver, req_local, 1, 0);

        /* Add to root_manifest if not present */
        int found = 0;
        for (SnManifestDep *d = root_manifest.direct; d; d = d->next) {
            if (strcmp(d->module, req_id) == 0) {
                found = 1;
                if (req_ver[0]) snprintf(d->version, sizeof(d->version), "%s", req_ver);
                snprintf(d->raw, sizeof(d->raw), "%s@%s", req_id, d->version[0] ? d->version : "1.0.0");
                break;
            }
        }
        if (!found) {
            SnManifestDep *dep = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
            snprintf(dep->module, sizeof(dep->module), "%s", req_id);
            snprintf(dep->version, sizeof(dep->version), "%s", req_ver[0] ? req_ver : "1.0.0");
            snprintf(dep->raw, sizeof(dep->raw), "%s@%s", req_id, dep->version);
            dep->next = root_manifest.direct;
            root_manifest.direct = dep;
            root_manifest.direct_count++;
        }
    }

    /* Add all existing direct deps from root_manifest */
    for (SnManifestDep *d = root_manifest.direct; d; d = d->next) {
        char d_id[256];
        char d_ver[64];
        char d_url[512];
        char d_rel[SNOVAC_PATH_MAX];
        normalize_module_info(d->raw, d->version, d_id, sizeof(d_id), d_ver, sizeof(d_ver),
                              d_url, sizeof(d_url), d_rel, sizeof(d_rel));

        char d_local[SNOVAC_PATH_MAX];
        snprintf(d_local, sizeof(d_local), "%s/%s", deps_root, d_rel);

        graph_add_node(&graph, d_id, d_url, d_ver, d_local, 1, 0);
    }

    /* Resolve graph starting from root node */
    int rc = graph_resolve_node(&graph, root_node);
    if (rc != 0) {
        if (graph.has_cycle) {
            fprintf(stderr, "error: %s\n", graph.cycle_msg);
        }
        manifest_free(&root_manifest);
        graph_free(&graph);
        return 1;
    }

    /* Reconstruct root manifest dependencies */
    manifest_free(&root_manifest);
    memset(&root_manifest.direct, 0, sizeof(root_manifest.direct));
    root_manifest.direct_count = 0;
    root_manifest.indirect = NULL;
    root_manifest.indirect_count = 0;

    /* Populate direct */
    for (DepNode *n = graph.nodes; n; n = n->next) {
        if (n->is_direct && !n->is_root) {
            SnManifestDep *d = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
            snprintf(d->module, sizeof(d->module), "%s", n->id);
            snprintf(d->version, sizeof(d->version), "%s", n->version[0] ? n->version : "1.0.0");
            snprintf(d->raw, sizeof(d->raw), "%s@%s", n->id, d->version);
            d->next = root_manifest.direct;
            root_manifest.direct = d;
            root_manifest.direct_count++;
        }
    }

    /* Populate indirect */
    for (DepEdge *e = graph.edges; e; e = e->next) {
        SnManifestDep *d = (SnManifestDep *)calloc(1, sizeof(SnManifestDep));
        d->is_edge = 1;
        snprintf(d->from, sizeof(d->from), "%s", e->from);
        snprintf(d->module, sizeof(d->module), "%s", e->to);
        snprintf(d->raw, sizeof(d->raw), "%s -> %s", e->from, e->to);
        d->next = root_manifest.indirect;
        root_manifest.indirect = d;
        root_manifest.indirect_count++;
    }

    /* Write updated manifest */
    if (!manifest_write(manifest_path, &root_manifest)) {
        manifest_free(&root_manifest);
        graph_free(&graph);
        return 1;
    }

    printf("snovac get: updated %s (%d direct, %d indirect dependencies)\n",
           manifest_path, root_manifest.direct_count, root_manifest.indirect_count);

    manifest_free(&root_manifest);
    graph_free(&graph);
    return 0;
}

int cmd_get(const char *url, const char *version) {
    return cmd_get_project(".", url, version);
}
