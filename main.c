/* main.c — snovac modular CLI driver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd_build.h"
#include "cmd_check.h"
#include "cmd_lex_parse.h"
#include "cmd_run.h"
#include "cmd_tidy.h"
#include "driver_utils.h"
#include "target.h"

typedef struct {
    const char *flag;
    int (*run)(const char *path, int dump);
    int dump;
} FileCommand;

static int cmd_run_adapter(const char *path, int dump) {
    (void)dump;
    return cmd_run(path);
}

static const FileCommand FILE_COMMANDS[] = {
    {"--emit=tokens",          cmd_lex,                 1},
    {"--check-lex",            cmd_lex,                 0},
    {"--emit=ast",             cmd_parse,               1},
    {"--check-parse",          cmd_parse,               0},
    {"--check-parse-project",  cmd_check_parse_project, 0},
    {"run",                    cmd_run_adapter,         0},
    {"check",                  cmd_check,               0},
};

int main(int argc, char **argv) {
    if (argc > 0 && argv[0]) {
        sn_set_exe_dir(argv[0]);
    }

    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("snovac %s\n", SNOVAC_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--target-info") == 0) {
        SnTargetInfo target = sn_target_get_active();
        sn_target_print_info(&target, stdout);
        return 0;
    }

    /* `tidy [--project] [<path>]`: cleans unused dependencies and generates/updates mod.sno */
    if (strcmp(argv[1], "tidy") == 0) {
        const char *proj_path = ".";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
                proj_path = argv[++i];
            } else if (strncmp(argv[i], "--project=", 10) == 0) {
                proj_path = argv[i] + 10;
            } else if (argv[i][0] != '-') {
                proj_path = argv[i];
            }
        }
        return cmd_tidy_project(proj_path);
    }

    /* `build [--project] <path> [-o <out>] [--target=<triple>] [--offline-cache[=<dir>]] [--runtime]`: compiles to standalone native binary */
    if (strcmp(argv[1], "build") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: build needs a file.snova or --project <path>\n");
            return 2;
        }
        int is_project = 0;
        int include_runtime = 0;
        const char *file_path = NULL;
        const char *out_path = NULL;
        const char *target_triple = NULL;
        const char *offline_cache = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0) {
                is_project = 1;
            } else if (strcmp(argv[i], "--runtime") == 0) {
                include_runtime = 1;
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out_path = argv[++i];
            } else if (strncmp(argv[i], "-o=", 3) == 0) {
                out_path = argv[i] + 3;
            } else if (strncmp(argv[i], "--target=", 9) == 0) {
                target_triple = argv[i] + 9;
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                target_triple = argv[++i];
            } else if (strncmp(argv[i], "--offline-cache=", 16) == 0) {
                offline_cache = argv[i] + 16;
            } else if (strcmp(argv[i], "--offline-cache") == 0) {
                offline_cache = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : ".snovalang/cache";
            } else if (argv[i][0] != '-' && !file_path) {
                file_path = argv[i];
            }
        }

        if (!file_path) {
            fprintf(stderr, "error: build needs a file.snova or project path\n");
            return 2;
        }
        if (is_project) {
            return cmd_build_project(file_path, out_path, target_triple, offline_cache, include_runtime);
        }
        return cmd_build(file_path, out_path, target_triple);
    }

    /* `run [--project] <path> [--offline-cache[=<dir>]]` */
    if (strcmp(argv[1], "run") == 0) {
        int is_project = 0;
        const char *path = NULL;
        const char *offline_cache = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0) {
                is_project = 1;
            } else if (strncmp(argv[i], "--offline-cache=", 16) == 0) {
                offline_cache = argv[i] + 16;
            } else if (strcmp(argv[i], "--offline-cache") == 0) {
                offline_cache = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : ".snovalang/cache";
            } else if (argv[i][0] != '-' && !path) {
                path = argv[i];
            }
        }

        if (!path) {
            fprintf(stderr, "error: run needs a path\n");
            return 2;
        }

        if (is_project) {
            return cmd_run_project_with_cache(path, offline_cache);
        }
        return cmd_run(path);
    }

    /* `check --project <path>` */
    if (strcmp(argv[1], "check") == 0 && argc > 2 && strcmp(argv[2], "--project") == 0) {
        int typecheck_bodies = 1;
        int argi = 3;
        if (argc > 3 && strcmp(argv[3], "--no-typecheck") == 0) {
            typecheck_bodies = 0;
            argi = 4;
        }
        if (argc <= argi) {
            fprintf(stderr, "error: check --project needs a path\n");
            return 2;
        }
        return cmd_check_project(argv[argi], typecheck_bodies);
    }

    for (size_t i = 0; i < sizeof(FILE_COMMANDS) / sizeof(FILE_COMMANDS[0]); i++) {
        const FileCommand *c = &FILE_COMMANDS[i];
        if (strcmp(argv[1], c->flag) != 0) {
            continue;
        }
        if (argc < 3) {
            fprintf(stderr, "error: %s needs a file\n", c->flag);
            return 2;
        }
        return c->run(argv[2], c->dump);
    }

    fprintf(stderr, "error: unknown option '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
