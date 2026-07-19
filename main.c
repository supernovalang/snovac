/* main.c — snovac driver (P0/P1).
 *
 * Implemented today: --version, --emit=tokens, --check-lex.
 * Parser, resolver, type checker and backends land in later phases; see
 * specs/20260719/snovac-c-toolchain/plan.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "lex.h"
#include "eval.h"
#include "parse.h"

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
            "  snovac run           <file.snova>   parse and execute\n",
            SNOVAC_VERSION);
}

static const char *vis_name(SnVisibility v) {
    switch (v) {
    case SN_VIS_PUBLIC: return "public ";
    case SN_VIS_PRIVATE: return "private ";
    case SN_VIS_PROTECTED: return "protected ";
    default: return "";
    }
}

static void print_type(const SnType *t) {
    if (!t) {
        printf("?");
        return;
    }
    if (t->kind == SN_TYPE_FUNC) {
        printf("(");
        for (size_t i = 0; i < t->params.len; i++) {
            if (i) printf(", ");
            print_type((const SnType *)t->params.items[i]);
        }
        printf(") -> ");
        print_type(t->ret);
        return;
    }
    printf("%s", t->name ? t->name : "?");
    if (t->args.len) {
        printf("<");
        for (size_t i = 0; i < t->args.len; i++) {
            if (i) printf(", ");
            print_type((const SnType *)t->args.items[i]);
        }
        printf(">");
    }
}

static void indent(int n) {
    for (int i = 0; i < n; i++) {
        printf("  ");
    }
}

static void print_decl(const SnDecl *d, int depth);

static void print_members(const SnList *l, int depth) {
    for (size_t i = 0; i < l->len; i++) {
        print_decl((const SnDecl *)l->items[i], depth);
    }
}

static void print_decl(const SnDecl *d, int depth) {
    indent(depth);
    switch (d->kind) {
    case SN_DECL_CLASS:     printf("%sclass %s", vis_name(d->vis), d->name); break;
    case SN_DECL_STRUCT:    printf("%sstruct %s", vis_name(d->vis), d->name); break;
    case SN_DECL_ENUM:      printf("%senum %s", vis_name(d->vis), d->name); break;
    case SN_DECL_INTERFACE: printf("%sinterface %s", vis_name(d->vis), d->name); break;
    case SN_DECL_METHOD:
    case SN_DECL_FUNC:
        printf("%s%s%s%s %s", vis_name(d->vis), d->is_static ? "static " : "",
               d->is_async ? "async " : "",
               d->kind == SN_DECL_METHOD ? "method" : "func", d->name);
        break;
    case SN_DECL_FIELD:   printf("%s%s %s", vis_name(d->vis), d->is_mutable ? "var" : "let", d->name); break;
    case SN_DECL_CONST:   printf("%sconst %s", vis_name(d->vis), d->name); break;
    case SN_DECL_VARIANT: printf("variant %s", d->name); break;
    case SN_DECL_TYPEALIAS: printf("%stype %s", vis_name(d->vis), d->name); break;
    }

    if (d->generics.len) {
        printf("<");
        for (size_t i = 0; i < d->generics.len; i++) {
            if (i) printf(", ");
            printf("%s", (const char *)d->generics.items[i]);
        }
        printf(">");
    }
    if (d->kind == SN_DECL_METHOD || d->kind == SN_DECL_FUNC ||
        d->kind == SN_DECL_VARIANT) {
        printf("(");
        for (size_t i = 0; i < d->params.len; i++) {
            const SnParam *pm = (const SnParam *)d->params.items[i];
            if (i) printf(", ");
            printf("%s: ", pm->name);
            print_type(pm->type);
            if (pm->def) printf(" = ...");
        }
        printf(")");
    }
    if (d->ret) {
        printf(": ");
        print_type(d->ret);
    }
    if (d->type) {
        printf(": ");
        print_type(d->type);
    }
    if (d->decorators.len) {
        printf("   [");
        for (size_t i = 0; i < d->decorators.len; i++) {
            if (i) printf(" ");
            printf("@%s", ((const SnDecorator *)d->decorators.items[i])->name);
        }
        printf("]");
    }
    if ((d->kind == SN_DECL_METHOD || d->kind == SN_DECL_FUNC) && !d->body) {
        printf("   (no body — expects @native)");
    }
    printf("\n");

    print_members(&d->variants, depth + 1);
    print_members(&d->members, depth + 1);
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
        if (unit.package) {
            printf("package %s\n", unit.package);
        }
        for (size_t i = 0; i < unit.imports.len; i++) {
            printf("import %s\n", (const char *)unit.imports.items[i]);
        }
        if (unit.imports.len || unit.package) {
            printf("\n");
        }
        for (size_t i = 0; i < unit.decls.len; i++) {
            print_decl((const SnDecl *)unit.decls.items[i], 0);
        }
    }

    int rc = diag.error_count > 0;
    if (diag.error_count > 0) {
        fprintf(stderr, "%d error%s in %s\n", diag.error_count,
                diag.error_count == 1 ? "" : "s", path);
    }

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
        fprintf(stderr, "%d error%s in %s\n", diag.error_count,
                diag.error_count == 1 ? "" : "s", path);
        rc = 1;
    } else {
        int code = sn_eval_run(&arena, &diag, &unit);
        rc = (code < 0) ? 1 : code;
    }

    sn_arena_free(&arena);
    free(src);
    return rc;
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
        for (size_t i = 0; i < toks.len; i++) {
            const SnToken *t = &toks.data[i];
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

    if (diag.error_count > 0) {
        fprintf(stderr, "%d error%s in %s\n", diag.error_count,
                diag.error_count == 1 ? "" : "s", path);
    }

    sn_arena_free(&arena);
    free(src);
    return rc ? 1 : 0;
}

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
    if (strcmp(argv[1], "--emit=tokens") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --emit=tokens needs a file\n");
            return 2;
        }
        return cmd_lex(argv[2], 1);
    }
    if (strcmp(argv[1], "--check-lex") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --check-lex needs a file\n");
            return 2;
        }
        return cmd_lex(argv[2], 0);
    }
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: run needs a file\n");
            return 2;
        }
        return cmd_run(argv[2]);
    }
    if (strcmp(argv[1], "--emit=ast") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --emit=ast needs a file\n");
            return 2;
        }
        return cmd_parse(argv[2], 1);
    }
    if (strcmp(argv[1], "--check-parse") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --check-parse needs a file\n");
            return 2;
        }
        return cmd_parse(argv[2], 0);
    }

    fprintf(stderr, "error: unknown option '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
