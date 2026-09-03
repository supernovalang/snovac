/* cmd_lex_parse.h — lexing, parsing, and token/ast dumping commands. */
#ifndef SNOVAC_CMD_LEX_PARSE_H
#define SNOVAC_CMD_LEX_PARSE_H

#include "token.h"
#include "lex.h"

void dump_tokens(const SnTokenVec *toks);
int cmd_lex(const char *path, int dump);
int cmd_parse(const char *path, int dump);

#endif /* SNOVAC_CMD_LEX_PARSE_H */
