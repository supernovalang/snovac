#include "package.h"

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
    static const char PREFIX[] = "builtin.metadata.";
    if (strncmp(name, PREFIX, sizeof(PREFIX) - 1u) == 0) {
        return "builtin.metadata";
    }
    return name;
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

    SnPackageNode *node = find_or_create(g, pf->package);
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
    /* Every diagnostic below (lexical errors, a missing `package` line) is
     * measured against THIS file, not whatever the sink was last pointed at
     * — the scan walks a whole tree. */
    SnDiagFile self = {path, src, len};
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
        SnSpan zero;
        memset(&zero, 0, sizeof(zero));
        zero.line = 1;
        zero.col = 1;
        sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_MISSING_PACKAGE_DECL, zero,
                     "%s: file has no `package` declaration", path);
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
    return n >= 6u && strcmp(path + n - 6u, ".snova") == 0;
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

/* ── linking + cycle detection ────────────────────────────────────────────
 *
 * A cycle among files of the SAME package is legal (plan.md §4.1: "é um
 * pacote só"), so same-package imports never become edges at all — the loop
 * below skips them before they reach the graph. */

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

                SnPackageNode *tnode = sn_pkggraph_find(g, target);
                if (!tnode) {
                    SnSpan *sp = SN_LIST_AT(pf->import_spans, SnSpan, i);
                    sn_diag_emit(g->diag, SN_DIAG_ERROR, SNOVA_IMPORT_NOT_FOUND,
                                 *sp, "package `%s` was not found", target);
                    continue;
                }

                int already = 0;
                for (size_t j = 0; j < node->edges.len; j++) {
                    if (SN_LIST_AT(node->edges, const char, j) == target) {
                        already = 1;
                        break;
                    }
                }
                if (!already) {
                    sn_list_push(g->arena, &node->edges, (void *)target);
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
