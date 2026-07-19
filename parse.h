#ifndef SNOVAC_PARSE_H
#define SNOVAC_PARSE_H

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "lex.h"

/* Parses a token stream into a compilation unit. Returns 0 when no syntax error
 * was reported. On error the parser recovers and keeps going, so `out` is still
 * populated (partially) and later phases can report more than one problem per
 * run. */
int sn_parse(SnArena *arena, SnDiagSink *diag, const SnTokenVec *toks,
             SnUnit *out);

#endif /* SNOVAC_PARSE_H */
