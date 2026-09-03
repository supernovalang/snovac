/* cmd_lex_parse.c — lexing, parsing, and token/ast dumping commands. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_lex_parse.h"
#include "arena.h"
#include "diag.h"
#include "dump.h"
#include "driver_utils.h"
#include "parse.h"

#include <stdio.h>
#include <stdlib.h>

void dump_tokens(const SnTokenVec *toks) {
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

int cmd_lex(const char *path, int dump) {
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

int cmd_parse(const char *path, int dump) {
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
