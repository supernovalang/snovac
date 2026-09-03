/* driver_utils.c — shared utilities for snovac CLI driver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "driver_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char g_exe_dir[SNOVAC_PATH_MAX] = {0};

void sn_set_exe_dir(const char *argv0) {
  if (argv0 && strchr(argv0, '/')) {
    dirname_into(argv0, g_exe_dir, sizeof(g_exe_dir));
  }
}

char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1u);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

void usage(FILE *out) {
  fprintf(
      out,
      "snovac %s — Snovalang compiler\n"
      "\n"
      "usage:\n"
      "  snovac --version\n"
      "  snovac --target-info                print detected OS/arch target and "
      "environment overrides\n"
      "  snovac --emit=tokens <file.snova>   dump the token stream\n"
      "  snovac --check-lex   <file.snova>   lex only; exit non-zero on error\n"
      "  snovac --emit=ast    <file.snova>   dump the parse tree\n"
      "  snovac --check-parse <file.snova>   lex+parse; exit non-zero on "
      "error\n"
      "  snovac run           <file.snova>   parse and execute\n"
      "  snovac build         <file.snova> [-o output] [--target=triple]\n"
      "                                       compile to standalone native "
      "executable\n"
      "  snovac check         <file.snova>   resolve + type-check (see llm.md: "
      "coverage is partial — no generics substitution yet)\n"
      "\n"
      "project-wide (every file the program is built from, not just the\n"
      "entry file — the source root is the nearest manifest's src/, its\n"
      "own directory otherwise, plus .snovalang/deps):\n"
      "  snovac --check-parse-project <path>  lex+parse the whole project\n"
      "  snovac check --project       <path>  + resolve and type-check\n"
      "  snovac check --project --no-typecheck <path>\n"
      "                                       everything except body type\n"
      "                                       checking (P4 generics are not\n"
      "                                       substituted yet)\n"
      "  snovac run --project         <path>  parse + execute across every\n"
      "                                       file in the project's own\n"
      "                                       source root (P3 preview; no\n"
      "                                       builtin/deps evaluation)\n"
      "\n"
      "environment variables:\n"
      "  SNOVA_TARGET_OS   target OS override (linux, darwin, windows, "
      "freebsd, ...)\n"
      "  SNOVA_TARGET_ARCH target architecture override (x86_64, aarch64, arm, "
      "...)\n"
      "  SNOVA_TARGET      target triple override (e.g. x86_64-linux-gnu)\n",
      SNOVAC_VERSION);
}

void report_errors(const SnDiagSink *diag, const char *path) {
  if (diag->error_count > 0) {
    fprintf(stderr, "%d error%s in %s\n", diag->error_count,
            diag->error_count == 1 ? "" : "s", path);
  }
}

int find_builtin_root(const char *start_dir, char *out, size_t out_sz) {
  char cur[SNOVAC_PATH_MAX + 64];
  snprintf(cur, sizeof(cur), "%s", start_dir);
  for (int i = 0; i < 8; i++) {
    char candidate[SNOVAC_PATH_MAX + 128];
    snprintf(candidate, sizeof(candidate), "%s/builtin", cur);
    struct stat st;
    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
      snprintf(out, out_sz, "%s", candidate);
      return 1;
    }
    char parent[SNOVAC_PATH_MAX + 64];
    snprintf(parent, sizeof(parent), "%s/..", cur);
    snprintf(cur, sizeof(cur), "%s", parent);
  }
  return 0;
}

void dirname_into(const char *path, char *out, size_t out_sz) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    snprintf(out, out_sz, ".");
    return;
  }
  size_t n = (size_t)(slash - path);
  if (n == 0) {
    n = 1; /* "/" */
  }
  if (n >= out_sz) {
    n = out_sz - 1;
  }
  memcpy(out, path, n);
  out[n] = '\0';
}

int path_is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int path_is_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Recursively creates `dir` and any missing parent directories (mkdir -p). */
static int mkdir_p(const char *dir) {
  if (!dir || !dir[0] || strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) {
    return 1;
  }
  struct stat st;
  if (stat(dir, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }

  char parent[SNOVAC_PATH_MAX];
  dirname_into(dir, parent, sizeof(parent));
  if (strcmp(parent, dir) != 0) {
    if (!mkdir_p(parent)) {
      return 0;
    }
  }

  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return 0;
  }
  return 1;
}

/* Ensures the parent directory of `path` exists, creating it (and any
 * missing ancestors) if necessary. Used before writing output/temporary
 * build artifacts so callers don't have to pre-create -o's directory. */
void ensure_parent_dir_exists(const char *path) {
  char dir[SNOVAC_PATH_MAX];
  dirname_into(path, dir, sizeof(dir));
  mkdir_p(dir);
}

void normalize_path_into(const char *path, char *out, size_t out_sz) {
  char resolved[SNOVAC_PATH_MAX];
  if (realpath(path, resolved) != NULL) {
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", path);
  }
}
