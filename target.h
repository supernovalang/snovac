/* target.h — OS and architecture target detection with environment overrides. */
#ifndef SNOVAC_TARGET_H
#define SNOVAC_TARGET_H

#include <stdio.h>

typedef enum {
    SN_OS_LINUX,
    SN_OS_DARWIN,
    SN_OS_WINDOWS,
    SN_OS_FREEBSD,
    SN_OS_OPENBSD,
    SN_OS_NETBSD,
    SN_OS_UNKNOWN
} SnTargetOS;

typedef enum {
    SN_ARCH_X86_64,
    SN_ARCH_AARCH64,
    SN_ARCH_ARM,
    SN_ARCH_RISCV64,
    SN_ARCH_X86,
    SN_ARCH_UNKNOWN
} SnTargetArch;

typedef struct {
    SnTargetOS os;
    SnTargetArch arch;
    char os_name[32];
    char arch_name[32];
    char triple[64];
    char exe_ext[16];
    char c_compiler[64];
    char cflags[256];
    char ldflags[256];
    int is_cross;
} SnTargetInfo;

void sn_target_init_default(SnTargetInfo *target);
void sn_target_resolve_from_env(SnTargetInfo *target);
int sn_target_parse_triple(SnTargetInfo *target, const char *triple);
SnTargetInfo sn_target_get_active(void);
void sn_target_print_info(const SnTargetInfo *target, FILE *out);

#endif /* SNOVAC_TARGET_H */
