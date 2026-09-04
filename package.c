#include "package.h"
#include "driver_utils.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "lex.h"

#define SN_PKG_PATH_MAX 4096

void sn_pkggraph_init(SnPackageGraph *g, SnArena *a, SnInternTable *it,
                       SnDiagSink *diag) {
    g->arena = a;
    g->intern = it;
    g->diag = diag;
    g->nodes = NULL;
    g->node_count = 0;
    g->file_count = 0;
    memset(&g->native_manifest, 0, sizeof(g->native_manifest));
    g->native_manifest_loaded = 0;
    sn_pkggraph_load_native_manifest(g, NULL);
}

void sn_pkggraph_load_native_manifest(SnPackageGraph *g, const char *builtin_dir) {
    if (builtin_dir && builtin_dir[0]) {
        char path[SN_PKG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/native-packages.list", builtin_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                                 line[n - 1] == ' ' || line[n - 1] == '\t')) {
                    line[--n] = '\0';
                }
                if (n == 0 || line[0] == '#') {
                    continue;
                }
                const char *name = sn_intern_cstr(g->intern, line);
                sn_list_push(g->arena, &g->native_manifest, (void *)name);
            }
            fclose(f);
            g->native_manifest_loaded = 1;
            return;
        }
    }
    // Embedded default native packages in pure C
    static const char *const DEFAULT_NATIVES[] = {
        "builtin.metadata.Documented",
        "builtin.syntax.Syntax"
    };
    for (size_t i = 0; i < sizeof(DEFAULT_NATIVES) / sizeof(DEFAULT_NATIVES[0]); i++) {
        const char *name = sn_intern_cstr(g->intern, DEFAULT_NATIVES[i]);
        sn_list_push(g->arena, &g->native_manifest, (void *)name);
    }
    g->native_manifest_loaded = 1;
}

SnPackageNode *sn_pkggraph_find(const SnPackageGraph *g, const char *name) {
    for (SnPackageNode *n = g->nodes; n; n = n->next) {
        if (n->name == name) {
            return n;
        }
    }
    return NULL;
}

static SnPackageNode *find_or_create(SnPackageGraph *g, const char *name) {
    SnPackageNode *n = sn_pkggraph_find(g, name);
    if (n) {
        return n;
    }
    n = (SnPackageNode *)sn_arena_calloc(g->arena, sizeof(SnPackageNode));
    n->name = name;
    n->next = g->nodes;
    g->nodes = n;
    g->node_count++;
    return n;
}

/* ── header-only scan ─────────────────────────────────────────────────────
 *
 * Reuses the real lexer (sn_lex) but not the real parser: header parsing only
 * needs `package <qualified>` once, then `import <qualified>` repeated, which
 * is far narrower than the parser's declaration grammar. Mirrors
 * parse_type.c's parse_qualified() (name ('.' name)*, where a "name" is any
 * identifier or contextual keyword — plan.md's fact 3) but works directly off
 * a SnTokenVec instead of the parser's private `P` state, so this module
 * stays decoupled from parse_internal.h. */

static const char *scan_qualified(SnArena *arena, const SnTokenVec *toks,
                                   size_t *pos, SnSpan *span_out) {
    const SnToken *t0 = &toks->data[*pos];
    if (t0->kind != SN_TOK_IDENT && !sn_tok_is_keyword(t0->kind)) {
        return NULL;
    }

    char buf[512];
    size_t n = strlen(t0->text);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1u;
    }
    memcpy(buf, t0->text, n);

    SnSpan span = t0->span;
    size_t p = *pos + 1u;

    while (toks->data[p].kind == SN_TOK_DOT &&
           (toks->data[p + 1u].kind == SN_TOK_IDENT ||
            sn_tok_is_keyword(toks->data[p + 1u].kind))) {
        p++; /* consume '.' */
        const SnToken *part = &toks->data[p];
        size_t pl = strlen(part->text);
        if (n + 1u + pl < sizeof(buf)) {
            buf[n++] = '.';
            memcpy(buf + n, part->text, pl);
            n += pl;
        }
        span.len = (part->span.offset + part->span.len) - span.offset;
        p++;
    }
    buf[n] = '\0';

    *pos = p;
    if (span_out) {
        *span_out = span;
    }
    return sn_arena_strndup(arena, buf, n);
}

/* `builtin.metadata` is the one bootstrap package whose declared name does
 * NOT repeat its file stem (`builtin/metadata/Documented.snova` declares
 * `package builtin.metadata`, where every other builtin declares
 * `builtin.<lower(Stem)>.<Stem>`). Its importers still spell it
 * `import builtin.metadata.Documented` — Types/DateTime/Collections all do,
 * and the file's own doc comment instructs it. That is deliberate, not a
 * corpus typo: the same exception is hardcoded in
 * `crates/snovalang/src/packages.rs::canonical_package_name` and in
 * `tools/launcher/snova.c::builtin_stdlib_package_name`, because `Documented`
 * is also one of the two native packages in packages_gen.c.
 *
 * Folding the import back to the declared name here (rather than aliasing the
 * node) keeps the exception in one place: link, package scopes and import
 * lookup all then match on the single name the file actually declares. */
static const char *canonical_import_name(const char *name) {
    if (!name) return name;
    static const char PREFIX[] = "builtin.metadata.";
    if (strncmp(name, PREFIX, sizeof(PREFIX) - 1u) == 0) {
        return "builtin.metadata";
    }
    return name;
}

static int find_nearest_manifest_rel_pkg(const char *file_path, char *out, size_t out_sz) {
    if (!file_path || !file_path[0]) return 0;
    char norm_path[SN_PKG_PATH_MAX];
    snprintf(norm_path, sizeof(norm_path), "%s", file_path);
    for (char *c = norm_path; *c; c++) {
        if (*c == '\\') *c = '/';
    }

    char cur_dir[SN_PKG_PATH_MAX];
    const char *last_slash = strrchr(norm_path, '/');
    if (!last_slash) return 0;
    size_t dlen = (size_t)(last_slash - norm_path);
    if (dlen >= sizeof(cur_dir)) dlen = sizeof(cur_dir) - 1;
    memcpy(cur_dir, norm_path, dlen);
    cur_dir[dlen] = '\0';

    char check_dir[SN_PKG_PATH_MAX];
    snprintf(check_dir, sizeof(check_dir), "%s", cur_dir);

    char manifest_dir[SN_PKG_PATH_MAX] = {0};
    int found_manifest = 0;

    for (int depth = 0; depth < 32; depth++) {
        char mod_cand[SN_PKG_PATH_MAX];
        snprintf(mod_cand, sizeof(mod_cand), "%s/mod.sno", check_dir);
        struct stat st;
        if (stat(mod_cand, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(manifest_dir, sizeof(manifest_dir), "%s", check_dir);
            found_manifest = 1;
            break;
        }
        char *p_slash = strrchr(check_dir, '/');
        if (!p_slash || p_slash == check_dir) break;
        *p_slash = '\0';
    }

    if (!found_manifest) return 0;

    /* Relative path between manifest_dir and cur_dir */
    size_t mlen = strlen(manifest_dir);
    if (strlen(cur_dir) < mlen) return 0;
    const char *rel = cur_dir + mlen;
    if (*rel == '/') rel++;
    if (!*rel) return 0;

    /* If rel starts with "src/", skip "src/" because src is conventional root */
    if (strncmp(rel, "src/", 4) == 0) rel += 4;
    else if (strcmp(rel, "src") == 0) return 0;

    /* Convert slashes to dots */
    char pkg_buf[SN_PKG_PATH_MAX];
    snprintf(pkg_buf, sizeof(pkg_buf), "%s", rel);
    for (char *c = pkg_buf; *c; c++) {
        if (*c == '/' || *c == '\\') *c = '.';
    }
    snprintf(out, out_sz, "%s", pkg_buf);
    return 1;
}

/* Scans one (package? import*) section starting at *pos and advances past
 * it, stopping at whatever ends the header — a declaration keyword, another
 * `package` (the next section), or EOF. Returns 1 if the section is usable
 * (had a package name), 0 otherwise (nothing added to the graph for it). */
static int scan_section(SnPackageGraph *g, const SnDiagFile *file,
                        const SnTokenVec *toks, size_t *pos) {
    SnArena *a = g->arena;
    SnPackageFile *pf = (SnPackageFile *)sn_arena_calloc(a, sizeof(SnPackageFile));
    pf->path = sn_arena_strndup(a, file->path, strlen(file->path));
    pf->src = file->src;
    pf->src_len = file->src_len;

    if (toks->data[*pos].kind == SN_TOK_PACKAGE) {
        (*pos)++;
        SnSpan span;
        const char *name = scan_qualified(a, toks, pos, &span);
        if (name) {
            pf->package = sn_intern_cstr(g->intern, name);
            pf->package_span = span;
        }
        if (toks->data[*pos].kind == SN_TOK_SEMI) {
            (*pos)++;
        }
    }

    while (toks->data[*pos].kind == SN_TOK_IMPORT) {
        (*pos)++;
        SnSpan span;
        const char *name = scan_qualified(a, toks, pos, &span);
        if (!name) {
            break; /* malformed import header; P1 owns rejecting this file at
                     * the full-parse gate, not this scanner */
        }
        if (toks->data[*pos].kind == SN_TOK_SEMI) {
            (*pos)++;
        }
        sn_list_push(a, &pf->imports,
                     (void *)sn_intern_cstr(g->intern, canonical_import_name(name)));
        SnSpan *sp = (SnSpan *)sn_arena_alloc(a, sizeof(SnSpan));
        *sp = span;
        sn_list_push(a, &pf->import_spans, sp);
    }

    if (!pf->package) {
        return 0;
    }

    /* Enforce physical directory path matching package declaration relative to nearest mod.sno checkpoint */
    char checkpoint_expected[SN_PKG_PATH_MAX] = {0};
    if (find_nearest_manifest_rel_pkg(file->path, checkpoint_expected, sizeof(checkpoint_expected))) {
        /* Check if declared package matches checkpoint_expected.
         * Allow case-insensitive comparison or matching suffix for module prefixing. */
        char normalized_declared[SN_PKG_PATH_MAX];
        snprintf(normalized_declared, sizeof(normalized_declared), "%s", pf->package);
        for (char *c = normalized_declared; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + ('a' - 'A'));
        }
        char normalized_expected[SN_PKG_PATH_MAX];
        snprintf(normalized_expected, sizeof(normalized_expected), "%s", checkpoint_expected);
        for (char *c = normalized_expected; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + ('a' - 'A'));
        }

        /* Check direct match or suffix match */
        size_t decl_len = strlen(normalized_declared);
        size_t exp_len = strlen(normalized_expected);
        int matched = 0;
        if (strcmp(normalized_declared, normalized_expected) == 0) {
            matched = 1;
        } else if (decl_len > exp_len && normalized_declared[decl_len - exp_len - 1] == '.' &&
                   strcmp(normalized_declared + (decl_len - exp_len), normalized_expected) == 0) {
            matched = 1;
        }

        if (!matched) {
            sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_PKG_PATH_MISMATCH, pf->package_span,
                         "package declaration '%s' does not match physical directory path relative to nearest 'mod.sno' checkpoint (expected '%s')",
                         pf->package, checkpoint_expected);
        }
    }

    SnPackageNode *node = find_or_create(g, pf->package);
    const char *p1 = strrchr(pf->path, '/');
    if (!p1) p1 = strrchr(pf->path, '\\');
    const char *name1 = p1 ? p1 + 1 : pf->path;
    for (SnPackageFile *existing = node->files; existing; existing = existing->next) {
        if (strcmp(pf->path, existing->path) == 0) {
            return 1;
        }
        const char *p2 = strrchr(existing->path, '/');
        if (!p2) p2 = strrchr(existing->path, '\\');
        const char *name2 = p2 ? p2 + 1 : existing->path;
        if (strcmp(name1, name2) == 0) {
            return 1;
        }
    }
    pf->next = node->files;
    node->files = pf;
    g->file_count++;
    return 1;
}

/* A file may contain more than one `package` section — parse.c documents
 * this precedent (tests/compile-fail/visibility_internal_cross_package.snova
 * puts a provider and a consumer package in one file to test cross-package
 * visibility without an import). Each section is scanned and grouped
 * independently; only the tokens between sections (declaration bodies) are
 * skipped, by fast-forwarding to the next `package`/`import`/EOF. */
static void skip_to_next_header_token(const SnTokenVec *toks, size_t *pos) {
    while (toks->data[*pos].kind != SN_TOK_EOF &&
           toks->data[*pos].kind != SN_TOK_PACKAGE &&
           toks->data[*pos].kind != SN_TOK_IMPORT) {
        (*pos)++;
    }
}

static void scan_header(SnPackageGraph *g, const char *path, const char *src,
                         size_t len) {
    SnArena *a = g->arena;
    char norm_path[SN_PKG_PATH_MAX];
    normalize_path_into(path, norm_path, sizeof(norm_path));
    SnDiagFile self = {norm_path, src, len};
    SnDiagFile outer = sn_diag_set_file(g->diag, self);

    SnTokenVec toks;
    memset(&toks, 0, sizeof(toks));
    sn_lex(a, g->diag, src, len, &toks); /* lexical errors reported via diag already */

    size_t pos = 0;
    int any_section = 0;
    while (toks.data[pos].kind != SN_TOK_EOF) {
        if (toks.data[pos].kind == SN_TOK_PACKAGE) {
            any_section |= scan_section(g, &self, &toks, &pos);
            continue;
        }
        if (toks.data[pos].kind == SN_TOK_IMPORT) {
            /* imports with no preceding `package` in this section: still
             * scan them (advances pos correctly) but they attach to nothing,
             * matching parse.c which only starts recording once a package is
             * seen. */
            (void)scan_section(g, &self, &toks, &pos);
            continue;
        }
        skip_to_next_header_token(&toks, &pos);
    }

    if (!any_section) {
        size_t path_len = strlen(path);
        int is_sno = (path_len >= 4u && strcmp(path + path_len - 4u, ".sno") == 0);
        if (is_sno) {
            /* .sno files are Snovalang script files; an explicit package header is optional.
             * They default to the implicit "main" script package. */
            SnPackageFile *pf = (SnPackageFile *)sn_arena_calloc(a, sizeof(SnPackageFile));
            pf->path = sn_arena_strndup(a, path, strlen(path));
            pf->src = src;
            pf->src_len = len;
            pf->package = sn_intern_cstr(g->intern, "main");

            size_t p = 0;
            while (toks.data[p].kind != SN_TOK_EOF) {
                if (toks.data[p].kind == SN_TOK_IMPORT) {
                    p++;
                    SnSpan span;
                    const char *name = scan_qualified(a, &toks, &p, &span);
                    if (name) {
                        if (toks.data[p].kind == SN_TOK_SEMI) p++;
                        sn_list_push(a, &pf->imports,
                                     (void *)sn_intern_cstr(g->intern, canonical_import_name(name)));
                        SnSpan *sp = (SnSpan *)sn_arena_alloc(a, sizeof(SnSpan));
                        *sp = span;
                        sn_list_push(a, &pf->import_spans, sp);
                    }
                } else {
                    p++;
                }
            }

            SnPackageNode *node = find_or_create(g, pf->package);
            pf->next = node->files;
            node->files = pf;
            g->file_count++;
        } else {
            SnSpan zero;
            memset(&zero, 0, sizeof(zero));
            zero.line = 1;
            zero.col = 1;
            sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_MISSING_PACKAGE_DECL, zero,
                         "%s: file has no `package` declaration", path);
        }
    }

    sn_diag_set_file(g->diag, outer);
}

/* ── directory walk ───────────────────────────────────────────────────────
 *
 * POSIX (dirent.h/sys/stat.h): matches the project's current platform
 * support (arm64 macOS primary, Linux backend planned — see
 * specs/20260719/snovac-c-toolchain/llm.md fact 6/7). No Windows path exists
 * anywhere else in snovac/ yet either. */

static char *read_source_file(SnArena *a, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)sn_arena_alloc(a, (size_t)n + 1u);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static int has_snova_suffix(const char *path) {
    size_t n = strlen(path);
    if (n >= 6u && strcmp(path + n - 6u, ".snova") == 0) {
        return 1;
    }
    if (n >= 4u && strcmp(path + n - 4u, ".sno") == 0) {
        const char *slash = strrchr(path, '/');
        const char *filename = slash ? slash + 1 : path;
        if (strcmp(filename, "mod.sno") == 0 || strcmp(filename, "snova.sno") == 0) {
            return 0;
        }
        return 1;
    }
    return 0;
}

/* Scans exactly one file, unlike sn_pkggraph_scan_root() which recursively
 * pulls in every `*.snova` under a directory — needed by callers (the CLI's
 * `check` command) that want ONE file's own declarations without silently
 * absorbing every sibling fixture in the same directory. Returns 1 if the
 * file was read and header-scanned, 0 if it couldn't be opened. */
int sn_pkggraph_scan_single_file(SnPackageGraph *g, const char *path) {
    size_t len = 0;
    char *src = read_source_file(g->arena, path, &len);
    if (!src) {
        return 0;
    }
    scan_header(g, path, src, len);
    return 1;
}

size_t sn_pkggraph_scan_root(SnPackageGraph *g, const char *root) {
    size_t count = 0;
    DIR *d = opendir(root);
    if (!d) {
        /* A missing/unreadable root is a configuration problem for whoever
         * assembles the root list (snova.toml, CLI args) — not this
         * function's job to diagnose. */
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char path[SN_PKG_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue; /* path too long to represent faithfully; skip rather
                       * than silently scan a truncated (wrong) path */
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            count += sn_pkggraph_scan_root(g, path);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !has_snova_suffix(path)) {
            continue;
        }

        count++;
        size_t flen = 0;
        char *src = read_source_file(g->arena, path, &flen);
        if (!src) {
            continue;
        }
        scan_header(g, path, src, flen);
    }
    closedir(d);
    return count;
}

size_t sn_pkggraph_scan_root_fallback(SnPackageGraph *g, const char *root) {
    size_t count = 0;
    DIR *d = opendir(root);
    if (!d) {
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char path[SN_PKG_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            count += sn_pkggraph_scan_root_fallback(g, path);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !has_snova_suffix(path)) {
            continue;
        }

        size_t flen = 0;
        char *src = read_source_file(g->arena, path, &flen);
        if (!src) {
            continue;
        }

        SnTokenVec toks;
        sn_lex(g->arena, g->diag, src, flen, &toks);
        size_t pos = 0;
        if (toks.len > 0 && toks.data[0].kind == SN_TOK_PACKAGE) {
            pos++;
            SnSpan span;
            const char *pname = scan_qualified(g->arena, &toks, &pos, &span);
            if (pname) {
                const char *interned = sn_intern_cstr(g->intern, pname);
                SnPackageNode *existing = sn_pkggraph_find(g, interned);
                if (existing && existing->files) {
                    if (strncmp(existing->files->path, root, strlen(root)) != 0) {
                        continue;
                    }
                }
            }
        }

        count++;
        scan_header(g, path, src, flen);
    }
    closedir(d);
    return count;
}

/* ── linking + cycle detection ────────────────────────────────────────────
 *
 * A cycle among files of the SAME package is legal (plan.md §4.1: "é um
 * pacote só"), so same-package imports never become edges at all — the loop
 * below skips them before they reach the graph. */

/* Resolves one import target to the package that provides it.
 *
 * An import names either a package outright or a SYMBOL inside one, and the
 * corpus uses both spellings against the same graph: `builtin.console.Console`
 * is a whole package (builtin/Console.snova declares exactly that), while
 * `builtin.auth.Auth.OAuth2` and `stdlib.sonar.serialization.Json` name a
 * symbol inside packages `builtin.auth.Auth` and `stdlib.sonar.serialization`.
 * Nothing in the syntax distinguishes the two, so resolution is longest-match:
 * try the whole dotted name, then drop trailing segments until a declared
 * package matches. Returns NULL only when no prefix is a package at all.
 *
 * Getting this wrong is not cosmetic — the symbol spelling is the common one
 * in real projects, and treating it as a missing package made SNOVA0050 fire
 * on correct code. */
static SnPackageNode *resolve_import_target(SnPackageGraph *g, const char *target) {
    SnPackageNode *exact = sn_pkggraph_find(g, target);
    if (exact) {
        return exact;
    }

    char prefix[SN_PKG_PATH_MAX];
    size_t n = strlen(target);
    if (n >= sizeof(prefix)) {
        return NULL;
    }
    memcpy(prefix, target, n + 1u);

    for (;;) {
        char *dot = strrchr(prefix, '.');
        if (!dot) {
            return NULL;
        }
        *dot = '\0';
        SnPackageNode *node = sn_pkggraph_find(g, sn_intern_cstr(g->intern, prefix));
        if (node) {
            return node;
        }
    }
}

/* Namespaces whose packages may legitimately have no `.snova` file anywhere
 * snovac can see, used only as a fallback when no native-package manifest
 * was loaded (sn_pkggraph_load_native_manifest was never called, or found no
 * `native-packages.list` — e.g. snovac invoked completely standalone,
 * outside any toolchain checkout). With no manifest, this module cannot
 * distinguish a real native-only package from a typo, so it declines to
 * judge rather than assert either way. When a manifest IS loaded,
 * native_package_known() below replaces this with an exact check instead. */
static int package_is_toolchain_provided(const char *name) {
    return strncmp(name, "builtin.", 8) == 0 || strncmp(name, "stdlib.", 7) == 0;
}

/* Whether `target` is a real, registered native package (one the toolchain
 * ships with no `.snova` source under any scanned root — e.g. metadata/
 * syntax packages backed directly by the Rust frontend). Prefers the exact
 * manifest generated by `scripts/gen-packages.sh` from the single source of
 * truth, `compiler/src/lsp/NativePackages.snova`; falls back to the old
 * blanket prefix rule only when that manifest could not be found at all, so
 * environments without a monorepo checkout keep their previous behavior. */
static int native_package_known(const SnPackageGraph *g, const char *target) {
    if (!g->native_manifest_loaded) {
        return package_is_toolchain_provided(target);
    }
    for (size_t i = 0; i < g->native_manifest.len; i++) {
        if (SN_LIST_AT(g->native_manifest, const char, i) == target) {
            return 1;
        }
    }
    return 0;
}

void sn_pkggraph_link(SnPackageGraph *g) {
    for (SnPackageNode *node = g->nodes; node; node = node->next) {
        for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
            /* Import spans belong to `pf`, not to whichever file the sink
             * happens to hold after the tree scan. */
            SnDiagFile self = {pf->path, pf->src, pf->src_len};
            SnDiagFile outer = sn_diag_set_file(g->diag, self);
            for (size_t i = 0; i < pf->imports.len; i++) {
                const char *target = SN_LIST_AT(pf->imports, const char, i);
                if (target == node->name) {
                    continue;
                }

                SnPackageNode *tnode = resolve_import_target(g, target);
                if (!tnode) {
                    if (native_package_known(g, target)) {
                        continue;
                    }
                    SnSpan *sp = SN_LIST_AT(pf->import_spans, SnSpan, i);
                    sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_IMPORT_NOT_FOUND,
                                 *sp, "package `%s` was not found", target);
                    continue;
                }
                /* A symbol import that resolves back to this same package is
                 * still not an edge — the exact-name check above only catches
                 * the whole-package spelling. */
                if (tnode->name == node->name) {
                    continue;
                }

                int already = 0;
                for (size_t j = 0; j < node->edges.len; j++) {
                    if (SN_LIST_AT(node->edges, const char, j) == tnode->name) {
                        already = 1;
                        break;
                    }
                }
                if (!already) {
                    sn_list_push(g->arena, &node->edges, (void *)tnode->name);
                }
            }
            sn_diag_set_file(g->diag, outer);
        }
    }
}

typedef struct {
    SnPackageNode **items;
    size_t depth;
} DfsPath;

static int dfs_cycle(SnPackageGraph *g, SnPackageNode *node, DfsPath *path,
                      SnList *cycle_out) {
    node->visited = 1;
    node->on_stack = 1;
    path->items[path->depth++] = node;

    for (size_t i = 0; i < node->edges.len; i++) {
        const char *target_name = SN_LIST_AT(node->edges, const char, i);
        SnPackageNode *target = sn_pkggraph_find(g, target_name);
        if (!target) {
            continue; /* sn_pkggraph_link() already reported unresolved edges */
        }
        if (target->on_stack) {
            size_t start = 0;
            for (size_t k = 0; k < path->depth; k++) {
                if (path->items[k] == target) {
                    start = k;
                    break;
                }
            }
            for (size_t k = start; k < path->depth; k++) {
                sn_list_push(g->arena, cycle_out, (void *)path->items[k]->name);
            }
            sn_list_push(g->arena, cycle_out, (void *)target->name);
            return 1;
        }
        if (!target->visited && dfs_cycle(g, target, path, cycle_out)) {
            return 1;
        }
    }

    node->on_stack = 0;
    path->depth--;
    return 0;
}

int sn_pkggraph_find_cycle(SnPackageGraph *g, SnList *cycle_out) {
    for (SnPackageNode *n = g->nodes; n; n = n->next) {
        n->visited = 0;
        n->on_stack = 0;
    }

    DfsPath path;
    path.items = (SnPackageNode **)sn_arena_alloc(
        g->arena, (g->node_count ? g->node_count : 1u) * sizeof(SnPackageNode *));
    path.depth = 0;

    for (SnPackageNode *n = g->nodes; n; n = n->next) {
        if (!n->visited && dfs_cycle(g, n, &path, cycle_out)) {
            return 1;
        }
    }
    return 0;
}
