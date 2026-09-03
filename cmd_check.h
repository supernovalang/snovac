/* cmd_check.h — single-file and project-wide check commands. */
#ifndef SNOVAC_CMD_CHECK_H
#define SNOVAC_CMD_CHECK_H

#include "arena.h"
#include "ast.h"
#include "check.h"
#include "diag.h"
#include "package.h"
#include "resolve.h"

typedef struct {
    const char *own_prefix;
} SnBodyCheckScope;

SnDiagFile begin_symbol_file(SnDiagSink *diag, const SnSymbol *sym);
void check_all_bodies(SnChecker *c, SnResolver *resolver, SnPackageGraph *graph,
                     SnArena *arena, const SnBodyCheckScope *scope);
void report_import_cycle(SnDiagSink *diag, SnPackageGraph *graph, const SnList *cycle);

int cmd_check(const char *path, int dump);
int cmd_check_project(const char *path, int typecheck_bodies);
int cmd_check_parse_project(const char *path, int dump);

#endif /* SNOVAC_CMD_CHECK_H */
