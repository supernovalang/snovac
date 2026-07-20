/* main.c — snovac driver.
 *
 * Implemented today: --version, --emit=tokens, --check-lex, --emit=ast,
 * --check-parse, run. Resolver, type checker and backends land in later
 * phases; see specs/20260719/snovac-c-toolchain/plan.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "dump.h"
#include "eval.h"
#include "lex.h"
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

static void report_errors(const SnDiagSink *diag, const char *path) {
    if (diag->error_count > 0) {
        fprintf(stderr, "%d error%s in %s\n", diag->error_count,
                diag->error_count == 1 ? "" : "s", path);
    }
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
