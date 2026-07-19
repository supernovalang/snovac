#ifndef SNOVAC_LEX_H
#define SNOVAC_LEX_H

#include "arena.h"
#include "diag.h"
#include "token.h"

typedef struct {
    SnToken *data;
    size_t len;
    size_t cap;
} SnTokenVec;

/* Tokenizes `src` into `out`. Returns 0 on success, non-zero when at least one
 * lexical error was reported. Always terminates the vector with SN_TOK_EOF, so
 * the parser can run even on a partially broken file. */
int sn_lex(SnArena *arena, SnDiagSink *diag, const char *src, size_t len,
           SnTokenVec *out);

#endif /* SNOVAC_LEX_H */
