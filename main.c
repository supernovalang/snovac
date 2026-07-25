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
            "coverage is partial — no generics substitution yet)\n",
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

/* Checks every FUNC/METHOD body reachable from `resolver`'s collected
 * packages and type member scopes. */
static void check_all_bodies(SnChecker *c, SnResolver *resolver, SnPackageGraph *graph,
                             SnArena *arena) {
    for (SnPackageScopeEntry *pe = resolver->packages; pe; pe = pe->next) {
        SnPackageNode *node = sn_pkggraph_find(graph, pe->package_name);
        SnList imports = node ? aggregate_imports(arena, node) : (SnList){0};
        for (size_t i = 0; i < pe->scope->nbuckets; i++) {
            for (SnSymbol *sym = pe->scope->buckets[i]; sym; sym = sym->next) {
                if ((sym->kind == SN_SYM_FUNC || sym->kind == SN_SYM_METHOD) &&
                    sym->decl->body) {
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
                if (sym->kind == SN_SYM_METHOD && sym->decl->body) {
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

    char builtin_dir[1024];
    if (find_builtin_root(dir, builtin_dir, sizeof(builtin_dir))) {
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
    check_all_bodies(&checker, &resolver, &graph, &arena);

    report_errors(&diag, path);
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
    {"--emit=tokens", cmd_lex,           1},
    {"--check-lex",   cmd_lex,           0},
    {"--emit=ast",    cmd_parse,         1},
    {"--check-parse", cmd_parse,         0},
    {"run",           cmd_run_adapter,   0},
    {"check",         cmd_check,         0},
};

int main(int argc, char **argv) {
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
