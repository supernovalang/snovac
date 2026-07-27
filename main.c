/* main.c — snovac driver.
 *
 * Implemented today: --version, --emit=tokens, --check-lex, --emit=ast,
 * --check-parse, run, check. Backends land in later phases; see
 * specs/20260719/snovac-c-toolchain/plan.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "arena.h"
#include "ast.h"
#include "check.h"
#include "diag.h"
#include "dump.h"
#include "eval.h"
#include "lex.h"
#include "package.h"
#include "parse.h"
#include "resolve.h"
#include "types.h"

#ifndef SNOVAC_VERSION
#define SNOVAC_VERSION "0.0.1-p1"
#endif

/* Own constant rather than <limits.h>'s PATH_MAX: this file only ever builds
 * paths out of a root plus a relative tail, and package.c already caps its
 * own walk at SN_PKG_PATH_MAX. */
#define SNOVAC_PATH_MAX 1024

static char *read_file(const char *path, size_t *out_len) {
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
    char *buf = (char *)malloc((size_t)n + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static void usage(FILE *out) {
    fprintf(out,
            "snovac %s — Snovalang compiler\n"
            "\n"
            "usage:\n"
            "  snovac --version\n"
            "  snovac --emit=tokens <file.snova>   dump the token stream\n"
            "  snovac --check-lex   <file.snova>   lex only; exit non-zero on error\n"
            "  snovac --emit=ast    <file.snova>   dump the parse tree\n"
            "  snovac --check-parse <file.snova>   lex+parse; exit non-zero on error\n"
            "  snovac run           <file.snova>   parse and execute\n"
            "  snovac check         <file.snova>   resolve + type-check (see llm.md: "
            "coverage is partial — no generics substitution yet)\n"
            "\n"
            "project-wide (every file the program is built from, not just the\n"
            "entry file — the source root is the nearest manifest's src/, its\n"
            "own directory otherwise, plus .snovalang/deps):\n"
            "  snovac --check-parse-project <path>  lex+parse the whole project\n"
            "  snovac check --project       <path>  + resolve and type-check\n"
            "  snovac check --project --no-typecheck <path>\n"
            "                                       everything except body type\n"
            "                                       checking (P4 generics are not\n"
            "                                       substituted yet)\n",
            SNOVAC_VERSION);
}

static void report_errors(const SnDiagSink *diag, const char *path) {
    if (diag->error_count > 0) {
        fprintf(stderr, "%d error%s in %s\n", diag->error_count,
                diag->error_count == 1 ? "" : "s", path);
    }
}

/* Best-effort: walk up from `start_dir` looking for a sibling `builtin/`
 * directory, so Option/Result/prelude resolve without the caller needing to
 * know this repo's layout. Not finding one just means the prelude stays
 * empty (sn_resolver_build_prelude() already handles that without crashing).
 */
static int find_builtin_root(const char *start_dir, char *out, size_t out_sz) {
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", start_dir);
    for (int i = 0; i < 8; i++) {
        char candidate[1200];
        snprintf(candidate, sizeof(candidate), "%s/builtin", cur);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, out_sz, "%s", candidate);
            return 1;
        }
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s/..", cur);
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

/* Directory of the snovac executable, when argv[0] carries a path. Set once
 * from main() because argv is not threaded through the command functions. */
static char g_exe_dir[1024];

static void dirname_into(const char *path, char *out, size_t out_sz) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_sz, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) {
        n = 1; /* "/" */
    }
    if (n >= out_sz) {
        n = out_sz - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

/* ── Project discovery ─────────────────────────────────────────────────────
 *
 * Deliberately mirrors crates/snovalang/src/modules/paths.rs so that snovac
 * analyses EXACTLY the file set the Rust Stage 0 already compiles into the
 * program — never a wider one. Widening it would make `snova check`/`run`
 * fail on files that are not part of the build, which is a worse bug than
 * the one this exists to fix.
 *
 * The rule, identical on both sides:
 *   source root = nearest ancestor holding a manifest, then its `src/` when
 *                 that is a directory, else the manifest directory itself;
 *                 falling back to the entry file's own parent directory when
 *                 no manifest exists anywhere above it.
 *   deps root   = `<manifest-dir>/.snovalang/deps`, only when populated.
 */

static const char *const MANIFEST_NAMES[] = {"snova.toml", "builtin.toml.Toml"};

static int path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Collapses `..` / `.` / symlinks so diagnostics print a path a human can
 * paste back into an editor — walking up with "%s/.." otherwise leaves
 * `src/a/b/../../../src` in every message. Leaves `path` untouched when it
 * does not resolve (a not-yet-created directory is the caller's problem to
 * report, not this helper's). */
static void normalize_path_into(const char *path, char *out, size_t out_sz) {
    char resolved[SNOVAC_PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        snprintf(out, out_sz, "%s", resolved);
    } else if (out != path) {
        snprintf(out, out_sz, "%s", path);
    }
}

/* Walks up from `start_dir` looking for a project manifest. Returns 1 and
 * writes the holding directory into `out` on success. */
static int find_manifest_dir(const char *start_dir, char *out, size_t out_sz) {
    char cur[SNOVAC_PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", start_dir);

    for (int depth = 0; depth < 32; depth++) {
        for (size_t i = 0; i < sizeof(MANIFEST_NAMES) / sizeof(MANIFEST_NAMES[0]); i++) {
            char candidate[SNOVAC_PATH_MAX + 64];
            snprintf(candidate, sizeof(candidate), "%s/%s", cur, MANIFEST_NAMES[i]);
            if (path_is_file(candidate)) {
                snprintf(out, out_sz, "%s", cur);
                return 1;
            }
        }
        char parent[SNOVAC_PATH_MAX + 8];
        snprintf(parent, sizeof(parent), "%s/..", cur);
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

typedef struct {
    char source_root[SNOVAC_PATH_MAX]; /* always set */
    char deps_root[SNOVAC_PATH_MAX];   /* "" when there is nothing vendored */
    int has_manifest;
} SnProject;

/* Resolves the project `path` belongs to. `path` may be a file or a
 * directory; either way `out->source_root` ends up as a directory. */
static void project_discover(const char *path, SnProject *out) {
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

    char src_dir[SNOVAC_PATH_MAX + 8];
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

/* Builtin modules for a project that is NOT inside this repository — walking
 * up from the project's own sources finds nothing there, so Option/Result and
 * every `import builtin.*` would be reported missing.
 *
 * Order: $SNOVA_BUILTIN_DIR (what the `snova` launcher sets, since it knows
 * where the toolchain is installed), then up from the project, then up from
 * snovac's own location (repo checkout: snovac/build/snovac -> ../../builtin).
 */
static int find_builtin_root_for_project(const char *source_root, char *out,
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

/* Feeds every root the project comprises into the package graph: its own
 * sources, its vendored dependencies, and the compiler's `builtin/` modules
 * (needed for the Option/Result prelude). Returns the number of `*.snova`
 * files found under the project's own source root — builtins and deps are
 * excluded from the count because callers report it as "files checked" and
 * those are not the caller's files. */
static size_t scan_project_roots(SnPackageGraph *graph, const SnProject *proj) {
    size_t own = sn_pkggraph_scan_root(graph, proj->source_root);
    if (proj->deps_root[0]) {
        sn_pkggraph_scan_root(graph, proj->deps_root);
    }
    char builtin_dir[SNOVAC_PATH_MAX];
    if (find_builtin_root_for_project(proj->source_root, builtin_dir,
                                      sizeof(builtin_dir))) {
        sn_pkggraph_scan_root(graph, builtin_dir);
    }
    return own;
}

/* Aggregates every import named by any file of `node` into one deduped
 * list. An approximation (per-file import lists aren't kept past
 * sn_resolver_collect(), see resolve.h) — fine for the common case this CLI
 * targets: one package, one or a few files, checked together. */
static SnList aggregate_imports(SnArena *a, SnPackageNode *node) {
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

/* Points `diag` at the file `sym` was declared in, so the body's diagnostics
 * carry that file's path and quote its source rather than the entry file's.
 * Returns the sink's previous file for the caller to restore. */
static SnDiagFile begin_symbol_file(SnDiagSink *diag, const SnSymbol *sym) {
    if (!sym->origin) {
        return diag->file;
    }
    return sn_diag_set_file(diag, *sym->origin);
}

/* Restricts check_all_bodies() to symbols declared under `own_prefix` (a
 * plain path prefix — the target file for `check <file>`, the project's
 * source_root for `check --project`). NULL/"" disables the restriction.
 *
 * Why: the package graph pulls in builtin/ (and, for --project, vendored
 * deps) purely so imports like `Console`/`Option` resolve by name — not
 * because the file being checked asked for builtin/'s own implementation to
 * be re-verified. Without this, `snovac check` on a trivial file reports
 * dozens of unrelated pre-existing errors from builtin/'s own half-finished
 * bodies, which is not what the user ran `check` to see (and
 * is a stdlib/L3 concern to fix, not something a compiler run should block
 * on). Bodies with no origin (sym->origin == NULL) are treated as "own" —
 * that only happens for symbols this checker itself introduces, never for
 * files pulled in from another root. */
typedef struct {
    const char *own_prefix;
} SnBodyCheckScope;

static int path_is_own(const SnBodyCheckScope *scope, const char *path) {
    if (!scope || !scope->own_prefix || !scope->own_prefix[0]) return 1;
    if (!path) return 1;
    return strncmp(path, scope->own_prefix, strlen(scope->own_prefix)) == 0;
}

/* Checks every FUNC/METHOD body reachable from `resolver`'s collected
 * packages and type member scopes, except those excluded by `scope`
 * (pass NULL to check everything). */
static void check_all_bodies(SnChecker *c, SnResolver *resolver, SnPackageGraph *graph,
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

static int cmd_check(const char *path, int dump) {
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

    /* find_builtin_root_for_project(), not the bare find_builtin_root(): the
     * latter only walks up from `dir` and gives up, so a file checked from
     * outside this repo (any mktemp'd scratch dir, e.g. every fixture in
     * tests/conformance/) never finds `builtin/` — every `import
     * builtin.*` then silently resolves to nothing (find_builtin_root's own
     * comment: "not finding one just means the prelude stays empty"), which
     * looks like an unrelated `SNOVA0023: not defined in this scope` at the
     * use site instead of a missing-builtin diagnostic. The project-aware
     * fallback chain (SNOVA_BUILTIN_DIR env var, then binary-relative via
     * g_exe_dir) already existed for `check --project`; single-file `check`
     * had no reason to be narrower. */
    char builtin_dir[1024];
    if (find_builtin_root_for_project(dir, builtin_dir, sizeof(builtin_dir))) {
        sn_pkggraph_scan_root(&graph, builtin_dir);
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

/* Renders a package import cycle found by sn_pkggraph_find_cycle(), which
 * deliberately emits nothing itself because a package-level cycle has no one
 * "right" file. Anchored at the `package` declaration of the cycle's first
 * node, in that node's own file. */
static void report_import_cycle(SnDiagSink *diag, SnPackageGraph *graph,
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

/* Project-wide `check`: the whole compilation unit goes through lexing,
 * parsing, name resolution and type checking — not just the entry file.
 *
 * `snovac check <file>` (single-file mode) exists for probing one file in
 * isolation and cannot see a syntax error in a sibling module; every file the
 * program is actually built from has to be analysed, which is what this
 * does. sn_resolver_collect() already re-lexes and fully parses every file in
 * the graph, so syntax errors anywhere in the project surface here too. */
static int cmd_check_project(const char *path, int typecheck_bodies) {
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

    /* sn_resolver_collect() lexes and fully parses every file in the graph,
     * so anything counted by now is a syntax or declaration error. Type
     * checking a unit that did not parse only produces cascades off recovered
     * AST holes — report what is already known and stop at this phase. */
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

/* Project-wide lex+parse gate. Stops at syntax: no resolution, no type
 * checking. Kept separate from cmd_check_project() because the two answer
 * different questions and a caller may want the syntax answer alone while
 * snovac's type checker is still partial (see
 * specs/20260719/snovac-p2-resolver-typechecker/llm.md). */
static int cmd_check_parse_project(const char *path, int dump) {
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

    /* Full lex+parse of every file the graph collected, in its own file
     * context so diagnostics carry the right path and quote the right line. */
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

static void dump_tokens(const SnTokenVec *toks) {
    for (size_t i = 0; i < toks->len; i++) {
        const SnToken *t = &toks->data[i];
        printf("%4u:%-3u %-16s", t->span.line, t->span.col,
               sn_tok_name(t->kind));
        if (t->kind == SN_TOK_IDENT || t->kind == SN_TOK_INT ||
            t->kind == SN_TOK_LONG || t->kind == SN_TOK_DOUBLE ||
            t->kind == SN_TOK_DECIMAL || t->kind == SN_TOK_STRING ||
            t->kind == SN_TOK_CHAR) {
            printf(" %s", t->text);
            if (t->kind == SN_TOK_STRING && t->has_interpolation) {
                printf("   [interpolated]");
            }
        }
        printf("\n");
    }
}

static int cmd_lex(const char *path, int dump) {
    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 2;
    }

    SnArena arena;
    sn_arena_init(&arena, 256 * 1024);

    SnDiagSink diag;
    sn_diag_init(&diag, path, src, len);

    SnTokenVec toks;
    int rc = sn_lex(&arena, &diag, src, len, &toks);

    if (dump) {
        dump_tokens(&toks);
    }
    report_errors(&diag, path);

    sn_arena_free(&arena);
    free(src);
    return rc ? 1 : 0;
}

static int cmd_parse(const char *path, int dump) {
    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 2;
    }

    SnArena arena;
    sn_arena_init(&arena, 256 * 1024);

    SnDiagSink diag;
    sn_diag_init(&diag, path, src, len);

    SnTokenVec toks;
    sn_lex(&arena, &diag, src, len, &toks);

    SnUnit unit;
    sn_parse(&arena, &diag, &toks, &unit);

    if (dump) {
        sn_dump_unit(&unit);
    }
    report_errors(&diag, path);

    int rc = diag.error_count > 0;
    sn_arena_free(&arena);
    free(src);
    return rc;
}

static int cmd_run(const char *path) {
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

/* A command that takes exactly one file argument. */
typedef struct {
    const char *flag;
    int (*run)(const char *path, int dump);
    int dump;
} FileCommand;

static int cmd_run_adapter(const char *path, int dump) {
    (void)dump;
    return cmd_run(path);
}

static const FileCommand FILE_COMMANDS[] = {
    {"--emit=tokens",          cmd_lex,                 1},
    {"--check-lex",            cmd_lex,                 0},
    {"--emit=ast",             cmd_parse,               1},
    {"--check-parse",          cmd_parse,               0},
    {"--check-parse-project",  cmd_check_parse_project, 0},
    {"run",                    cmd_run_adapter,         0},
    {"check",                  cmd_check,               0},
};

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
        dirname_into(argv[0], g_exe_dir, sizeof(g_exe_dir));
    }

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("snovac %s\n", SNOVAC_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }

    /* `check --project <path>`: the project-wide form of `check`. Handled
     * before the table because FILE_COMMANDS entries take their path at
     * argv[2] and this one carries a flag there. */
    if (strcmp(argv[1], "check") == 0 && argc > 2 &&
        strcmp(argv[2], "--project") == 0) {
        int typecheck_bodies = 1;
        int argi = 3;
        if (argc > 3 && strcmp(argv[3], "--no-typecheck") == 0) {
            typecheck_bodies = 0;
            argi = 4;
        }
        if (argc <= argi) {
            fprintf(stderr, "error: check --project needs a path\n");
            return 2;
        }
        return cmd_check_project(argv[argi], typecheck_bodies);
    }

    for (size_t i = 0; i < sizeof(FILE_COMMANDS) / sizeof(FILE_COMMANDS[0]);
         i++) {
        const FileCommand *c = &FILE_COMMANDS[i];
        if (strcmp(argv[1], c->flag) != 0) {
            continue;
        }
        if (argc < 3) {
            fprintf(stderr, "error: %s needs a file\n", c->flag);
            return 2;
        }
        return c->run(argv[2], c->dump);
    }

    fprintf(stderr, "error: unknown option '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
