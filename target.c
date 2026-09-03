/* target.c — OS and architecture target detection with environment overrides. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#define HOST_OS SN_OS_LINUX
#define HOST_OS_STR "linux"
#elif defined(__APPLE__) && defined(__MACH__)
#define HOST_OS SN_OS_DARWIN
#define HOST_OS_STR "darwin"
#elif defined(_WIN32) || defined(_WIN64)
#define HOST_OS SN_OS_WINDOWS
#define HOST_OS_STR "windows"
#elif defined(__FreeBSD__)
#define HOST_OS SN_OS_FREEBSD
#define HOST_OS_STR "freebsd"
#elif defined(__OpenBSD__)
#define HOST_OS SN_OS_OPENBSD
#define HOST_OS_STR "openbsd"
#elif defined(__NetBSD__)
#define HOST_OS SN_OS_NETBSD
#define HOST_OS_STR "netbsd"
#else
#define HOST_OS SN_OS_UNKNOWN
#define HOST_OS_STR "unknown"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define HOST_ARCH SN_ARCH_X86_64
#define HOST_ARCH_STR "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HOST_ARCH SN_ARCH_AARCH64
#define HOST_ARCH_STR "aarch64"
#elif defined(__arm__) || defined(_M_ARM)
#define HOST_ARCH SN_ARCH_ARM
#define HOST_ARCH_STR "arm"
#elif defined(__riscv) && (__riscv_xlen == 64)
#define HOST_ARCH SN_ARCH_RISCV64
#define HOST_ARCH_STR "riscv64"
#elif defined(__i386__) || defined(_M_IX86)
#define HOST_ARCH SN_ARCH_X86
#define HOST_ARCH_STR "x86"
#else
#define HOST_ARCH SN_ARCH_UNKNOWN
#define HOST_ARCH_STR "unknown"
#endif

static void update_target_flags(SnTargetInfo *target) {
    if (target->os == SN_OS_WINDOWS) {
        snprintf(target->exe_ext, sizeof(target->exe_ext), ".exe");
        snprintf(target->cflags, sizeof(target->cflags), "-std=c11 -O2");
        snprintf(target->ldflags, sizeof(target->ldflags), "-lws2_32");
    } else if (target->os == SN_OS_DARWIN) {
        target->exe_ext[0] = '\0';
        snprintf(target->cflags, sizeof(target->cflags), "-std=c11 -O2 -pthread");
        snprintf(target->ldflags, sizeof(target->ldflags), "-lpthread -lm");
    } else {
        target->exe_ext[0] = '\0';
        snprintf(target->cflags, sizeof(target->cflags), "-std=c11 -O2 -pthread");
        snprintf(target->ldflags, sizeof(target->ldflags), "-pthread -lm -ldl");
    }

    /* Set default compiler if not specified */
    if (target->c_compiler[0] == '\0') {
        const char *env_cc = getenv("SNOVA_CC");
        if (!env_cc) env_cc = getenv("CC");
        if (env_cc && env_cc[0]) {
            snprintf(target->c_compiler, sizeof(target->c_compiler), "%s", env_cc);
        } else {
            snprintf(target->c_compiler, sizeof(target->c_compiler), "cc");
        }
    }

    snprintf(target->triple, sizeof(target->triple), "%s-%s%s",
             target->arch_name,
             (target->os == SN_OS_WINDOWS) ? "pc-windows" :
             (target->os == SN_OS_DARWIN) ? "apple-darwin" :
             (target->os == SN_OS_FREEBSD) ? "unknown-freebsd" : "unknown-linux",
             (target->os == SN_OS_WINDOWS) ? "-gnu" :
             (target->os == SN_OS_LINUX) ? "-gnu" : "");
}

void sn_target_init_default(SnTargetInfo *target) {
    memset(target, 0, sizeof(*target));
    target->os = HOST_OS;
    target->arch = HOST_ARCH;
    snprintf(target->os_name, sizeof(target->os_name), "%s", HOST_OS_STR);
    snprintf(target->arch_name, sizeof(target->arch_name), "%s", HOST_ARCH_STR);
    target->is_cross = 0;
    update_target_flags(target);
}

int sn_target_parse_triple(SnTargetInfo *target, const char *triple) {
    if (!triple || !triple[0]) return 0;

    if (strstr(triple, "x86_64") || strstr(triple, "amd64")) {
        target->arch = SN_ARCH_X86_64;
        snprintf(target->arch_name, sizeof(target->arch_name), "x86_64");
    } else if (strstr(triple, "aarch64") || strstr(triple, "arm64")) {
        target->arch = SN_ARCH_AARCH64;
        snprintf(target->arch_name, sizeof(target->arch_name), "aarch64");
    } else if (strstr(triple, "arm")) {
        target->arch = SN_ARCH_ARM;
        snprintf(target->arch_name, sizeof(target->arch_name), "arm");
    } else if (strstr(triple, "riscv64")) {
        target->arch = SN_ARCH_RISCV64;
        snprintf(target->arch_name, sizeof(target->arch_name), "riscv64");
    } else if (strstr(triple, "i386") || strstr(triple, "i686") || strstr(triple, "x86")) {
        target->arch = SN_ARCH_X86;
        snprintf(target->arch_name, sizeof(target->arch_name), "x86");
    }

    if (strstr(triple, "linux")) {
        target->os = SN_OS_LINUX;
        snprintf(target->os_name, sizeof(target->os_name), "linux");
    } else if (strstr(triple, "darwin") || strstr(triple, "macos") || strstr(triple, "apple") || strstr(triple, "ios")) {
        target->os = SN_OS_DARWIN;
        snprintf(target->os_name, sizeof(target->os_name), "darwin");
    } else if (strstr(triple, "windows") || strstr(triple, "mingw") || strstr(triple, "msvc") || strstr(triple, "win32")) {
        target->os = SN_OS_WINDOWS;
        snprintf(target->os_name, sizeof(target->os_name), "windows");
    } else if (strstr(triple, "freebsd")) {
        target->os = SN_OS_FREEBSD;
        snprintf(target->os_name, sizeof(target->os_name), "freebsd");
    } else if (strstr(triple, "openbsd")) {
        target->os = SN_OS_OPENBSD;
        snprintf(target->os_name, sizeof(target->os_name), "openbsd");
    } else if (strstr(triple, "netbsd")) {
        target->os = SN_OS_NETBSD;
        snprintf(target->os_name, sizeof(target->os_name), "netbsd");
    }

    if (target->os != HOST_OS || target->arch != HOST_ARCH) {
        target->is_cross = 1;
    }
    update_target_flags(target);
    return 1;
}

void sn_target_resolve_from_env(SnTargetInfo *target) {
    sn_target_init_default(target);

    /* Check full target triple env var */
    const char *env_target = getenv("SNOVA_TARGET");
    if (!env_target) env_target = getenv("TARGET");
    if (env_target && env_target[0]) {
        sn_target_parse_triple(target, env_target);
        return;
    }

    /* Check target OS env var */
    const char *env_os = getenv("SNOVA_TARGET_OS");
    if (!env_os) env_os = getenv("TARGET_OS");
    if (env_os && env_os[0]) {
        if (strcmp(env_os, "linux") == 0) {
            target->os = SN_OS_LINUX;
            snprintf(target->os_name, sizeof(target->os_name), "linux");
        } else if (strcmp(env_os, "darwin") == 0 || strcmp(env_os, "macos") == 0) {
            target->os = SN_OS_DARWIN;
            snprintf(target->os_name, sizeof(target->os_name), "darwin");
        } else if (strcmp(env_os, "windows") == 0 || strcmp(env_os, "win32") == 0) {
            target->os = SN_OS_WINDOWS;
            snprintf(target->os_name, sizeof(target->os_name), "windows");
        } else if (strcmp(env_os, "freebsd") == 0) {
            target->os = SN_OS_FREEBSD;
            snprintf(target->os_name, sizeof(target->os_name), "freebsd");
        } else if (strcmp(env_os, "openbsd") == 0) {
            target->os = SN_OS_OPENBSD;
            snprintf(target->os_name, sizeof(target->os_name), "openbsd");
        } else if (strcmp(env_os, "netbsd") == 0) {
            target->os = SN_OS_NETBSD;
            snprintf(target->os_name, sizeof(target->os_name), "netbsd");
        }
    }

    /* Check target Arch env var */
    const char *env_arch = getenv("SNOVA_TARGET_ARCH");
    if (!env_arch) env_arch = getenv("TARGET_ARCH");
    if (env_arch && env_arch[0]) {
        if (strcmp(env_arch, "x86_64") == 0 || strcmp(env_arch, "amd64") == 0) {
            target->arch = SN_ARCH_X86_64;
            snprintf(target->arch_name, sizeof(target->arch_name), "x86_64");
        } else if (strcmp(env_arch, "aarch64") == 0 || strcmp(env_arch, "arm64") == 0) {
            target->arch = SN_ARCH_AARCH64;
            snprintf(target->arch_name, sizeof(target->arch_name), "aarch64");
        } else if (strcmp(env_arch, "arm") == 0) {
            target->arch = SN_ARCH_ARM;
            snprintf(target->arch_name, sizeof(target->arch_name), "arm");
        } else if (strcmp(env_arch, "riscv64") == 0) {
            target->arch = SN_ARCH_RISCV64;
            snprintf(target->arch_name, sizeof(target->arch_name), "riscv64");
        } else if (strcmp(env_arch, "x86") == 0 || strcmp(env_arch, "i386") == 0 || strcmp(env_arch, "i686") == 0) {
            target->arch = SN_ARCH_X86;
            snprintf(target->arch_name, sizeof(target->arch_name), "x86");
        }
    }

    if (target->os != HOST_OS || target->arch != HOST_ARCH) {
        target->is_cross = 1;
    }
    update_target_flags(target);
}

SnTargetInfo sn_target_get_active(void) {
    SnTargetInfo target;
    sn_target_resolve_from_env(&target);
    return target;
}

void sn_target_print_info(const SnTargetInfo *target, FILE *out) {
    fprintf(out, "snovac target configuration:\n");
    fprintf(out, "  Host OS:       %s\n", HOST_OS_STR);
    fprintf(out, "  Host Arch:     %s\n", HOST_ARCH_STR);
    fprintf(out, "  Target OS:     %s%s\n", target->os_name, (target->os == HOST_OS ? " (native default)" : " (overridden)"));
    fprintf(out, "  Target Arch:   %s%s\n", target->arch_name, (target->arch == HOST_ARCH ? " (native default)" : " (overridden)"));
    fprintf(out, "  Target Triple: %s\n", target->triple);
    fprintf(out, "  C Compiler:    %s\n", target->c_compiler);
    fprintf(out, "  CFLAGS:        %s\n", target->cflags);
    fprintf(out, "  LDFLAGS:       %s\n", target->ldflags);
}
