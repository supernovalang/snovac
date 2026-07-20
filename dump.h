/* dump.h — human-readable AST printing for `snovac --emit=ast`.
 *
 * A debugging view, not a serialization format: nothing parses it back, so it
 * favours readability over round-tripping.
 */
#ifndef SNOVAC_DUMP_H
#define SNOVAC_DUMP_H

#include "ast.h"

void sn_dump_type(const SnType *t);
void sn_dump_decl(const SnDecl *d, int depth);
void sn_dump_unit(const SnUnit *unit);

#endif /* SNOVAC_DUMP_H */
