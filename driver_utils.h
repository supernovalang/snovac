/* driver_utils.h — shared utilities for snovac CLI driver. */
#ifndef SNOVAC_DRIVER_UTILS_H
#define SNOVAC_DRIVER_UTILS_H

#include "diag.h"
#include <stddef.h>
#include <stdio.h>

#ifndef SNOVAC_VERSION
#define SNOVAC_VERSION "0.0.1-p1"
#endif

#define SNOVAC_PATH_MAX 1024

extern char g_exe_dir[SNOVAC_PATH_MAX];

void sn_set_exe_dir(const char *argv0);
char *read_file(const char *path, size_t *out_len);
void usage(FILE *out);
void report_errors(const SnDiagSink *diag, const char *path);
void dirname_into(const char *path, char *out, size_t out_sz);
void normalize_path_into(const char *path, char *out, size_t out_sz);
int path_is_dir(const char *path);
int path_is_file(const char *path);
void ensure_parent_dir_exists(const char *path);
int find_builtin_root(const char *start_dir, char *out, size_t out_sz);

#endif /* SNOVAC_DRIVER_UTILS_H */
