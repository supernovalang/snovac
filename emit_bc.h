/* emit_bc.h — AST to SnBC bytecode compiler. */
#ifndef SNOVAC_EMIT_BC_H
#define SNOVAC_EMIT_BC_H

#include "ast.h"
#include "diag.h"
#include "snbc.h"

int sn_emit_bytecode(SnArena *arena, SnDiagSink *diag, const SnUnit *unit, SnBCUnit *out);

#endif /* SNOVAC_EMIT_BC_H */
