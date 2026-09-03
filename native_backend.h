/* native_backend.h — Native executable compiler backend for Snovalang. */
#ifndef SNOVAC_NATIVE_BACKEND_H
#define SNOVAC_NATIVE_BACKEND_H

#include "snbc.h"
#include "target.h"

int sn_native_compile(const SnBCUnit *bc, const SnTargetInfo *target, const char *output_path);

#endif /* SNOVAC_NATIVE_BACKEND_H */
