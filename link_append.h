/* link_append.h — Standalone executable generator for Snovalang. */
#ifndef SNOVAC_LINK_APPEND_H
#define SNOVAC_LINK_APPEND_H

#include "snbc.h"

/* Generates a standalone native executable from the compiled bytecode. */
int sn_build_executable(const SnBCUnit *bc, const char *output_path);

#endif /* SNOVAC_LINK_APPEND_H */
