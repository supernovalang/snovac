#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sn_vm_init(SnVM *vm, SnBCUnit *unit) {
    vm->unit = unit;
    vm->frame_count = 0;
    vm->stack_top = vm->stack;
    vm->global_count = 128;
    vm->globals = (SnVal *)calloc(vm->global_count, sizeof(SnVal));
}

void sn_vm_free(SnVM *vm) {
    for (SnVal *s = vm->stack; s < vm->stack_top; s++) {
        sn_val_release(*s);
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        sn_val_release(vm->globals[i]);
    }
    free(vm->globals);
}

static inline void push(SnVM *vm, SnVal val) {
    *vm->stack_top++ = val;
}

static inline SnVal pop(SnVM *vm) {
    return *(--vm->stack_top);
}

static inline SnVal peek(SnVM *vm, int distance) {
    return vm->stack_top[-1 - distance];
}

static inline uint32_t read_u32(uint8_t **ip) {
    uint32_t val = (uint32_t)((*ip)[0]) |
                   ((uint32_t)((*ip)[1]) << 8) |
                   ((uint32_t)((*ip)[2]) << 16) |
                   ((uint32_t)((*ip)[3]) << 24);
    *ip += 4;
    return val;
}

static inline int32_t read_i32(uint8_t **ip) {
    return (int32_t)read_u32(ip);
}

static inline int64_t read_i64(uint8_t **ip) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)((*ip)[i]) << (i * 8));
    }
    *ip += 8;
    return (int64_t)val;
}

static inline double read_double(uint8_t **ip) {
    union {
        double d;
        uint64_t u;
    } cvt;
    cvt.u = 0;
    for (int i = 0; i < 8; i++) {
        cvt.u |= ((uint64_t)((*ip)[i]) << (i * 8));
    }
    *ip += 8;
    return cvt.d;
}

int sn_vm_interpret(SnVM *vm) {
    if (!vm->unit || vm->unit->function_count == 0) {
        return 0;
    }
    SnFunctionChunk *main_fn = vm->unit->functions[vm->unit->main_func_idx];
    if (!main_fn) {
        return 0;
    }

    SnCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->fn = main_fn;
    frame->ip = main_fn->chunk.code;
    frame->slots = vm->stack;

    /* Pad stack for local variables */
    for (size_t i = 0; i < main_fn->local_count; i++) {
        push(vm, sn_val_unit());
    }

    for (;;) {
        SnOpcode op = (SnOpcode)(*frame->ip++);
        switch (op) {
        case OP_NOP:
            break;
        case OP_CONST_INT: {
            int64_t val = read_i64(&frame->ip);
            push(vm, sn_val_int(val));
            break;
        }
        case OP_CONST_DOUBLE: {
            double val = read_double(&frame->ip);
            push(vm, sn_val_double(val));
            break;
        }
        case OP_CONST_STRING: {
            uint32_t s_idx = read_u32(&frame->ip);
            const char *str = vm->unit->string_pool.strings[s_idx];
            push(vm, sn_val_string(str, strlen(str)));
            break;
        }
        case OP_CONST_BOOL: {
            uint8_t b = *frame->ip++;
            push(vm, sn_val_bool(b != 0));
            break;
        }
        case OP_CONST_UNIT: {
            push(vm, sn_val_unit());
            break;
        }
        case OP_POP: {
            SnVal v = pop(vm);
            sn_val_release(v);
            break;
        }
        case OP_DUP: {
            SnVal v = peek(vm, 0);
            sn_val_retain(v);
            push(vm, v);
            break;
        }
        case OP_GET_LOCAL: {
            uint32_t slot = read_u32(&frame->ip);
            SnVal v = frame->slots[slot];
            sn_val_retain(v);
            push(vm, v);
            break;
        }
        case OP_SET_LOCAL: {
            uint32_t slot = read_u32(&frame->ip);
            SnVal v = peek(vm, 0);
            sn_val_retain(v);
            sn_val_release(frame->slots[slot]);
            frame->slots[slot] = v;
            break;
        }
        case OP_GET_GLOBAL: {
            uint32_t slot = read_u32(&frame->ip);
            SnVal v = vm->globals[slot];
            sn_val_retain(v);
            push(vm, v);
            break;
        }
        case OP_SET_GLOBAL: {
            uint32_t slot = read_u32(&frame->ip);
            SnVal v = peek(vm, 0);
            sn_val_retain(v);
            sn_val_release(vm->globals[slot]);
            vm->globals[slot] = v;
            break;
        }
        case OP_ADD: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            if (a.tag == VAL_STRING && b.tag == VAL_STRING) {
                SnStringObj *sa = (SnStringObj *)a.as.obj;
                SnStringObj *sb = (SnStringObj *)b.as.obj;
                size_t total = sa->len + sb->len;
                char *buf = (char *)malloc(total + 1);
                memcpy(buf, sa->chars, sa->len);
                memcpy(buf + sa->len, sb->chars, sb->len);
                buf[total] = '\0';
                SnVal res = sn_val_string(buf, total);
                free(buf);
                sn_val_release(a);
                sn_val_release(b);
                push(vm, res);
            } else if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
                double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
                double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
                sn_val_release(a);
                sn_val_release(b);
                push(vm, sn_val_double(da + db));
            } else {
                int64_t ia = a.as.i;
                int64_t ib = b.as.i;
                sn_val_release(a);
                sn_val_release(b);
                push(vm, sn_val_int(ia + ib));
            }
            break;
        }
        case OP_SUB: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
                double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
                double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
                push(vm, sn_val_double(da - db));
            } else {
                push(vm, sn_val_int(a.as.i - b.as.i));
            }
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_MUL: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
                double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
                double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
                push(vm, sn_val_double(da * db));
            } else {
                push(vm, sn_val_int(a.as.i * b.as.i));
            }
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_DIV: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {
                double da = a.tag == VAL_DOUBLE ? a.as.d : (double)a.as.i;
                double db = b.tag == VAL_DOUBLE ? b.as.d : (double)b.as.i;
                push(vm, sn_val_double(db == 0.0 ? 0.0 : da / db));
            } else {
                push(vm, sn_val_int(b.as.i == 0 ? 0 : a.as.i / b.as.i));
            }
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_MOD: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_int(b.as.i == 0 ? 0 : a.as.i % b.as.i));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_NEG: {
            SnVal a = pop(vm);
            if (a.tag == VAL_DOUBLE) {
                push(vm, sn_val_double(-a.as.d));
            } else {
                push(vm, sn_val_int(-a.as.i));
            }
            sn_val_release(a);
            break;
        }
        case OP_NOT: {
            SnVal a = pop(vm);
            push(vm, sn_val_bool(!a.as.b));
            sn_val_release(a);
            break;
        }
        case OP_EQ: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_bool(sn_val_equal(a, b)));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_NE: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_bool(!sn_val_equal(a, b)));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_LT: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_bool(a.as.i < b.as.i));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_LE: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_bool(a.as.i <= b.as.i));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_GT: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_bool(a.as.i > b.as.i));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_GE: {
            SnVal b = pop(vm);
            SnVal a = pop(vm);
            push(vm, sn_val_bool(a.as.i >= b.as.i));
            sn_val_release(a);
            sn_val_release(b);
            break;
        }
        case OP_JUMP: {
            int32_t offset = read_i32(&frame->ip);
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            int32_t offset = read_i32(&frame->ip);
            SnVal cond = pop(vm);
            if (!cond.as.b) {
                frame->ip += offset;
            }
            sn_val_release(cond);
            break;
        }
        case OP_JUMP_IF_TRUE: {
            int32_t offset = read_i32(&frame->ip);
            SnVal cond = pop(vm);
            if (cond.as.b) {
                frame->ip += offset;
            }
            sn_val_release(cond);
            break;
        }
        case OP_CALL: {
            uint32_t fn_idx = read_u32(&frame->ip);
            uint32_t argc = read_u32(&frame->ip);
            SnFunctionChunk *callee = vm->unit->functions[fn_idx];
            if (vm->frame_count >= SN_VM_FRAMES_MAX) {
                fprintf(stderr, "Stack overflow\n");
                return 1;
            }
            SnCallFrame *nframe = &vm->frames[vm->frame_count++];
            nframe->fn = callee;
            nframe->ip = callee->chunk.code;
            nframe->slots = vm->stack_top - argc;
            /* Expand local slots if function has more locals than args */
            for (size_t i = argc; i < callee->local_count; i++) {
                push(vm, sn_val_unit());
            }
            frame = nframe;
            break;
        }
        case OP_RETURN: {
            SnVal res = pop(vm);
            vm->frame_count--;
            if (vm->frame_count == 0) {
                int exit_code = res.tag == VAL_INT ? (int)res.as.i : 0;
                sn_val_release(res);
                return exit_code;
            }
            /* Clean up frame stack slots */
            while (vm->stack_top > frame->slots) {
                sn_val_release(pop(vm));
            }
            push(vm, res);
            frame = &vm->frames[vm->frame_count - 1];
            break;
        }
        case OP_NEW_OBJ: {
            uint32_t class_id = read_u32(&frame->ip);
            uint32_t field_count = read_u32(&frame->ip);
            SnVal inst = sn_val_instance(class_id, field_count);
            SnInstanceObj *io = (SnInstanceObj *)inst.as.obj;
            for (int i = (int)field_count - 1; i >= 0; i--) {
                io->fields[i] = pop(vm);
            }
            push(vm, inst);
            break;
        }
        case OP_GET_FIELD: {
            uint32_t f_idx = read_u32(&frame->ip);
            SnVal target = pop(vm);
            if (target.tag == VAL_OBJECT && target.as.obj) {
                SnInstanceObj *io = (SnInstanceObj *)target.as.obj;
                if (f_idx < io->field_count) {
                    SnVal fval = io->fields[f_idx];
                    sn_val_retain(fval);
                    sn_val_release(target);
                    push(vm, fval);
                    break;
                }
            }
            sn_val_release(target);
            push(vm, sn_val_unit());
            break;
        }
        case OP_SET_FIELD: {
            uint32_t f_idx = read_u32(&frame->ip);
            SnVal val = pop(vm);
            SnVal target = pop(vm);
            if (target.tag == VAL_OBJECT && target.as.obj) {
                SnInstanceObj *io = (SnInstanceObj *)target.as.obj;
                if (f_idx < io->field_count) {
                    sn_val_release(io->fields[f_idx]);
                    sn_val_retain(val);
                    io->fields[f_idx] = val;
                }
            }
            sn_val_release(target);
            push(vm, val);
            break;
        }
        case OP_NEW_ARRAY: {
            uint32_t count = read_u32(&frame->ip);
            SnVal arr = sn_val_array(count);
            SnArrayObj *ao = (SnArrayObj *)arr.as.obj;
            ao->count = count;
            for (int i = (int)count - 1; i >= 0; i--) {
                ao->items[i] = pop(vm);
            }
            push(vm, arr);
            break;
        }
        case OP_GET_INDEX: {
            SnVal idx = pop(vm);
            SnVal target = pop(vm);
            if (target.tag == VAL_ARRAY && target.as.obj && idx.tag == VAL_INT) {
                SnArrayObj *ao = (SnArrayObj *)target.as.obj;
                int64_t i = idx.as.i;
                if (i >= 0 && (size_t)i < ao->count) {
                    SnVal item = ao->items[i];
                    sn_val_retain(item);
                    push(vm, item);
                } else {
                    push(vm, sn_val_unit());
                }
            } else {
                push(vm, sn_val_unit());
            }
            sn_val_release(target);
            sn_val_release(idx);
            break;
        }
        case OP_SET_INDEX: {
            SnVal val = pop(vm);
            SnVal idx = pop(vm);
            SnVal target = pop(vm);
            if (target.tag == VAL_ARRAY && target.as.obj && idx.tag == VAL_INT) {
                SnArrayObj *ao = (SnArrayObj *)target.as.obj;
                int64_t i = idx.as.i;
                if (i >= 0 && (size_t)i < ao->count) {
                    sn_val_release(ao->items[i]);
                    sn_val_retain(val);
                    ao->items[i] = val;
                }
            }
            sn_val_release(target);
            sn_val_release(idx);
            push(vm, val);
            break;
        }
        case OP_VARIANT: {
            uint32_t tag_id = read_u32(&frame->ip);
            uint32_t payload_count = read_u32(&frame->ip);
            uint32_t name_idx = read_u32(&frame->ip);
            const char *name = vm->unit->string_pool.strings[name_idx];
            SnVal var = sn_val_variant(tag_id, name, payload_count);
            SnVariantObj *vo = (SnVariantObj *)var.as.obj;
            for (int i = (int)payload_count - 1; i >= 0; i--) {
                vo->payloads[i] = pop(vm);
            }
            push(vm, var);
            break;
        }
        case OP_IS_VARIANT: {
            uint32_t tag_id = read_u32(&frame->ip);
            SnVal v = pop(vm);
            bool match = (v.tag == VAL_VARIANT && v.as.obj && ((SnVariantObj *)v.as.obj)->tag_id == tag_id);
            push(vm, sn_val_bool(match));
            sn_val_release(v);
            break;
        }
        case OP_UNWRAP_VARIANT: {
            uint32_t p_idx = read_u32(&frame->ip);
            SnVal v = pop(vm);
            if (v.tag == VAL_VARIANT && v.as.obj) {
                SnVariantObj *vo = (SnVariantObj *)v.as.obj;
                if (p_idx < vo->payload_count) {
                    SnVal payload = vo->payloads[p_idx];
                    sn_val_retain(payload);
                    sn_val_release(v);
                    push(vm, payload);
                    break;
                }
            }
            sn_val_release(v);
            push(vm, sn_val_unit());
            break;
        }
        case OP_PRINT: {
            uint8_t is_newline = *frame->ip++;
            SnVal v = pop(vm);
            sn_val_print(v, is_newline != 0);
            sn_val_release(v);
            break;
        }
        case OP_HALT:
            return 0;
        default:
            fprintf(stderr, "Unknown opcode: %u\n", (unsigned)op);
            return 1;
        }
    }
}
