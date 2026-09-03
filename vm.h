/* vm.h — Snovalang stack-based bytecode virtual machine. */
#ifndef SNOVAC_VM_H
#define SNOVAC_VM_H

#include "snbc.h"
#include "value.h"

#define SN_VM_STACK_MAX 4096
#define SN_VM_FRAMES_MAX 256

typedef struct {
    SnFunctionChunk *fn;
    uint8_t *ip;
    SnVal *slots;
} SnCallFrame;

typedef struct {
    SnBCUnit *unit;
    SnCallFrame frames[SN_VM_FRAMES_MAX];
    size_t frame_count;

    SnVal stack[SN_VM_STACK_MAX];
    SnVal *stack_top;

    SnVal *globals;
    size_t global_count;
} SnVM;

void sn_vm_init(SnVM *vm, SnBCUnit *unit);
void sn_vm_free(SnVM *vm);
int sn_vm_interpret(SnVM *vm);

#endif /* SNOVAC_VM_H */
