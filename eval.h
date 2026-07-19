/* eval.h — tree-walking evaluator over the AST.
 *
 * This is the first execution engine that is actually driven by parsed
 * structure. The Rust Stage 0 it replaces scanned raw source text for
 * substrings (`line.contains(".map(")`) and never built a tree; every
 * behaviour here comes from SnDecl/SnStmt/SnExpr nodes instead.
 *
 * Scope today (P3 preview): functions, classes with fields and methods,
 * let/var, assignment, arithmetic and comparison, if/while, return,
 * string concatenation and interpolation, and the Console intrinsics.
 * Generics, pattern matching, async/pulsars and the remaining ~590 @native
 * intrinsics are later phases — see specs/20260719/snovac-c-toolchain/plan.md.
 *
 * Memory: every value lives in the arena and is released in one shot when the
 * program ends. Reference counting arrives with the bytecode VM (P3 proper).
 */
#ifndef SNOVAC_EVAL_H
#define SNOVAC_EVAL_H

#include "arena.h"
#include "ast.h"
#include "diag.h"

/* Runs `unit`'s `main`. Returns the process exit code main produced, or a
 * negative value when the program could not be run at all. */
int sn_eval_run(SnArena *arena, SnDiagSink *diag, const SnUnit *unit);

#endif /* SNOVAC_EVAL_H */
