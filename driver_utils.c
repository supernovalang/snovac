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
#include <direct.h>
#include <io.h>
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
  if (n > 0 && n < out_sz) {
    for (size_t i = 0; i < (size_t)n; i++) {
      if (out[i] == '\\') {
        out[i] = '/';
      }
    }
    return 1;
  }
  return 0;
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
#if defined(_WIN32)
    const char *sep = strchr(p, ';');
#else
    const char *sep = strchr(p, ':');
#endif
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
#if defined(_WIN32)
      snprintf(candidate, sizeof(candidate), "%s/%s.exe", dir, argv0);
      if (path_is_file(candidate)) {
        normalize_path_into(candidate, out, out_sz);
        return 1;
      }
#endif
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

  if (argv0 && (strchr(argv0, '/') || strchr(argv0, '\\'))) {
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
      "sncli %s - Snovalang Toolchain & Compiler\n"
      "\n"
      "usage:\n"
      "  sncli --version\n"
      "  sncli --target-info                print detected OS/arch target and "
      "environment overrides\n"
      "  sncli --emit=tokens <file.snova>   dump the token stream\n"
      "  sncli --check-lex   <file.snova>   lex only; exit non-zero on error\n"
      "  sncli --emit=ast    <file.snova>   dump the parse tree\n"
      "  sncli --check-parse <file.snova>   lex+parse; exit non-zero on "
      "error\n"
      "  sncli run           <file.snova>   parse and execute\n"
      "  sncli build         <file.snova> [-o output] [--target=triple]\n"
      "                                       compile to standalone native "
      "executable\n"
      "  sncli check         <file.snova>   resolve + type-check (see llm.md: "
      "coverage is partial - no generics substitution yet)\n"
      "\n"
      "dependencies and package management:\n"
      "  sncli get           [<repo-url>] [--version=<ver>] [--project=<path>]\n"
      "                                       fetch and resolve transitive dependencies\n"
      "                                       into .snovalang/deps, update mod.sno\n"
      "  sncli tidy          [--project] [<path>]\n"
      "                                       prune unused dependencies and sync mod.sno\n"
      "\n"
      "project-wide (every file the program is built from, not just the\n"
      "entry file - the source root is the nearest manifest's src/, its\n"
      "own directory otherwise, plus .snovalang/deps):\n"
      "  sncli --check-parse-project <path>  lex+parse the whole project\n"
      "  sncli check --project       <path>  + resolve and type-check\n"
      "  sncli check --project --no-typecheck <path>\n"
      "                                       everything except body type\n"
      "                                       checking (P4 generics are not\n"
      "                                       substituted yet)\n"
      "  sncli run --project         <path>  parse + execute across every\n"
      "                                       file in the project's own\n"
      "                                       source root (P3 preview; no\n"
      "                                       builtin/deps evaluation)\n"
      "  sncli build --project       <path> [-o output] [--target=triple] [--runtime]\n"
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
  const char *env_std = getenv("SNOVA_STD_PATH");
  if (env_std && env_std[0] && path_is_dir(env_std)) {
    normalize_path_into(env_std, out, out_sz);
    return 1;
  }

  /* 1. Prefer canonical installed standard library (~/.snovalang/std/src) */
#if defined(_WIN32)
  const char *home = getenv("USERPROFILE");
#else
  const char *home = getenv("HOME");
#endif
  if (home && home[0]) {
    char installed_std[SNOVAC_PATH_MAX + 128];
    snprintf(installed_std, sizeof(installed_std), "%s/.snovalang/std/src", home);
    if (path_is_dir(installed_std)) {
      normalize_path_into(installed_std, out, out_sz);
      return 1;
    }
  }

  /* 2. Check project dependency .snovalang/deps/github.com/supernovalang/snova-std/src */
  if (start_dir && start_dir[0]) {
    char dep_std[SNOVAC_PATH_MAX + 128];
    snprintf(dep_std, sizeof(dep_std), "%s/.snovalang/deps/github.com/supernovalang/snova-std/src", start_dir);
    if (path_is_dir(dep_std)) {
      normalize_path_into(dep_std, out, out_sz);
      return 1;
    }
  }

  /* 3. Sibling/development directory search (normalized, no relative .. in output) */
  char cur[SNOVAC_PATH_MAX + 64];
  normalize_path_into(start_dir, cur, sizeof(cur));
  for (int i = 0; i < 8; i++) {
    char candidate_std[SNOVAC_PATH_MAX + 128];
    snprintf(candidate_std, sizeof(candidate_std), "%s/snova-std/src", cur);
    if (path_is_dir(candidate_std)) {
      normalize_path_into(candidate_std, out, out_sz);
      return 1;
    }

    char candidate_std_direct[SNOVAC_PATH_MAX + 128];
    snprintf(candidate_std_direct, sizeof(candidate_std_direct), "%s/src", cur);
    char test_file[SNOVAC_PATH_MAX + 160];
    snprintf(test_file, sizeof(test_file), "%s/Snova/Std", candidate_std_direct);
    if (path_is_dir(test_file)) {
      normalize_path_into(candidate_std_direct, out, out_sz);
      return 1;
    }

    char candidate[SNOVAC_PATH_MAX + 128];
    snprintf(candidate, sizeof(candidate), "%s/builtin", cur);
    struct stat st;
    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
      normalize_path_into(candidate, out, out_sz);
      return 1;
    }
    char parent[SNOVAC_PATH_MAX + 64];
    snprintf(parent, sizeof(parent), "%s/..", cur);
    normalize_path_into(parent, cur, sizeof(cur));
  }

  return 0;
}

void dirname_into(const char *path, char *out, size_t out_sz) {
  const char *slash = strrchr(path, '/');
#if defined(_WIN32)
  const char *bslash = strrchr(path, '\\');
  if (bslash && (!slash || bslash > slash)) {
    slash = bslash;
  }
#endif
  if (!slash) {
    snprintf(out, out_sz, ".");
    return;
  }
  size_t n = (size_t)(slash - path);
  if (n == 0) {
    n = 1; /* "/" or "\" */
  }
#if defined(_WIN32)
  if (n == 2 && path[1] == ':') {
    n = 3;
  }
#endif
  if (n >= out_sz) {
    n = out_sz - 1;
  }
  memcpy(out, path, n);
  out[n] = '\0';
}

int path_is_dir(const char *path) {
  if (!path || !path[0]) return 0;
  char norm[SNOVAC_PATH_MAX];
  normalize_path_into(path, norm, sizeof(norm));
  struct stat st;
  return stat(norm, &st) == 0 && S_ISDIR(st.st_mode);
}

int path_is_file(const char *path) {
  if (!path || !path[0]) return 0;
  char norm[SNOVAC_PATH_MAX];
  normalize_path_into(path, norm, sizeof(norm));
  struct stat st;
  return stat(norm, &st) == 0 && S_ISREG(st.st_mode);
}

/* Recursively creates `dir` and any missing parent directories (mkdir -p). */
static int mkdir_p(const char *dir) {
  if (!dir || !dir[0] || strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0 ||
      strcmp(dir, "\\") == 0) {
    return 1;
  }
#if defined(_WIN32)
  if (strlen(dir) == 2 && dir[1] == ':') {
    return 1;
  }
  if (strlen(dir) == 3 && dir[1] == ':' && (dir[2] == '/' || dir[2] == '\\')) {
    return 1;
  }
#endif
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

#if defined(_WIN32)
  if (_mkdir(dir) != 0 && errno != EEXIST) {
    return 0;
  }
#else
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return 0;
  }
#endif
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
  if (!path || !path[0]) {
    if (out && out_sz > 0) out[0] = '\0';
    return;
  }
  char resolved[SNOVAC_PATH_MAX];
#if defined(_WIN32)
  const char *src = path;
  char msys_buf[SNOVAC_PATH_MAX];
  /* Handle MSYS/Git-Bash/Cygwin absolute paths like "/c/Users/..." -> "C:/Users/..." */
  if (path[0] == '/' && isalpha((unsigned char)path[1]) && (path[2] == '/' || path[2] == '\0')) {
    snprintf(msys_buf, sizeof(msys_buf), "%c:%s", (char)toupper((unsigned char)path[1]), path + 2);
    src = msys_buf;
  }
  if (_fullpath(resolved, src, sizeof(resolved)) != NULL) {
    for (char *p = resolved; *p; p++) {
      if (*p == '\\') {
        *p = '/';
      }
    }
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", src);
  }
#else
  if (realpath(path, resolved) != NULL) {
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", path);
  }
#endif
}
