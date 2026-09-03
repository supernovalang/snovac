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

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

char g_exe_dir[SNOVAC_PATH_MAX] = {0};

/* Resolves the absolute path of the currently running executable using the
 * most reliable OS-specific API available. Returns 1 and fills `out` on
 * success, 0 otherwise (leaving `out` untouched). This does NOT depend on
 * argv[0], which is unreliable: when a binary is found via $PATH (e.g.
 * `snovac build ...` after `make install`), the shell execve()s the
 * resolved path but usually still passes argv[0] as the bare command name
 * typed by the user, with no '/' in it at all. */
static int resolve_exe_path(char *out, size_t out_sz) {
#if defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_sz);
  return (n > 0 && n < out_sz) ? 1 : 0;
#elif defined(__APPLE__)
  char tmp[SNOVAC_PATH_MAX];
  uint32_t tmp_size = (uint32_t)sizeof(tmp);
  if (_NSGetExecutablePath(tmp, &tmp_size) != 0) {
    return 0;
  }
  if (realpath(tmp, out) == NULL) {
    snprintf(out, out_sz, "%s", tmp);
  }
  return 1;
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
  if (n < 0) {
    return 0;
  }
  out[n] = '\0';
  return 1;
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  size_t len = out_sz;
  if (sysctl(mib, 4, out, &len, NULL, 0) != 0) {
    return 0;
  }
  return 1;
#else
  (void)out;
  (void)out_sz;
  return 0;
#endif
}

/* Last-resort fallback for platforms without a "current executable" API:
 * manually search $PATH for argv0, the way a POSIX shell would. */
static int resolve_exe_path_via_path_env(const char *argv0, char *out,
                                         size_t out_sz) {
  if (!argv0 || !argv0[0]) {
    return 0;
  }
  const char *path_env = getenv("PATH");
  if (!path_env) {
    return 0;
  }
  const char *p = path_env;
  while (p && *p) {
    const char *sep = strchr(p, ':');
    size_t len = sep ? (size_t)(sep - p) : strlen(p);
    if (len > 0 && len < SNOVAC_PATH_MAX - 1) {
      char dir[SNOVAC_PATH_MAX];
      memcpy(dir, p, len);
      dir[len] = '\0';
      char candidate[SNOVAC_PATH_MAX];
      snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0);
      if (path_is_file(candidate)) {
        normalize_path_into(candidate, out, out_sz);
        return 1;
      }
    }
    p = sep ? sep + 1 : NULL;
  }
  return 0;
}

void sn_set_exe_dir(const char *argv0) {
  char exe_path[SNOVAC_PATH_MAX];

  if (resolve_exe_path(exe_path, sizeof(exe_path)) && exe_path[0]) {
    dirname_into(exe_path, g_exe_dir, sizeof(g_exe_dir));
    return;
  }

  if (argv0 && strchr(argv0, '/')) {
    normalize_path_into(argv0, exe_path, sizeof(exe_path));
    dirname_into(exe_path, g_exe_dir, sizeof(g_exe_dir));
    return;
  }

  if (resolve_exe_path_via_path_env(argv0, exe_path, sizeof(exe_path))) {
    dirname_into(exe_path, g_exe_dir, sizeof(g_exe_dir));
    return;
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
      "dependencies and package management:\n"
      "  snovac get           [<repo-url>] [--version=<ver>] [--project=<path>]\n"
      "                                       fetch and resolve transitive dependencies\n"
      "                                       into .snovalang/deps, update mod.sno\n"
      "  snovac tidy          [--project] [<path>]\n"
      "                                       prune unused dependencies and sync mod.sno\n"
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
      "  snovac build --project       <path> [-o output] [--target=triple] [--runtime]\n"
      "                                       compile project into standalone native binary\n"
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
