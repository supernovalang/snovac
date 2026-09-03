/* snbc.h — Snovalang Bytecode (SnBC) definitions. */
#ifndef SNOVAC_SNBC_H
#define SNOVAC_SNBC_H

#include <stdint.h>
#include <stddef.h>

#define SNBC_MAGIC   0x43424E53 /* "SNBC" */
#define SNBC_VERSION 1

typedef enum {
    OP_NOP = 0,
    OP_CONST_INT,     /* [int64_t] */
    OP_CONST_DOUBLE,  /* [double] */
    OP_CONST_STRING,  /* [uint32_t string_index] */
    OP_CONST_BOOL,    /* [uint8_t] */
    OP_CONST_UNIT,
    OP_POP,
    OP_DUP,

    OP_GET_LOCAL,     /* [uint32_t index] */
    OP_SET_LOCAL,     /* [uint32_t index] */
    OP_GET_GLOBAL,    /* [uint32_t index] */
    OP_SET_GLOBAL,    /* [uint32_t index] */

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,
    OP_NOT,
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_SHL,
    OP_SHR,

    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,

    OP_JUMP,          /* [int32_t offset] */
    OP_JUMP_IF_FALSE, /* [int32_t offset] */
    OP_JUMP_IF_TRUE,  /* [int32_t offset] */

    OP_CALL,          /* [uint32_t fn_idx, uint32_t argc] */
    OP_INVOKE,        /* [uint32_t method_name_idx, uint32_t argc] */
    OP_RETURN,

    OP_NEW_OBJ,       /* [uint32_t class_idx, uint32_t field_count] */
    OP_GET_FIELD,     /* [uint32_t field_idx] */
    OP_SET_FIELD,     /* [uint32_t field_idx] */
    OP_NEW_ARRAY,     /* [uint32_t elem_count] */
    OP_GET_INDEX,
    OP_SET_INDEX,

    OP_VARIANT,       /* [uint32_t tag_id, uint32_t payload_count] */
    OP_IS_VARIANT,    /* [uint32_t tag_id] */
    OP_UNWRAP_VARIANT,/* [uint32_t payload_idx] */

    OP_PRINT,         /* [uint8_t is_newline] */
    OP_HALT
} SnOpcode;

typedef struct {
    uint8_t *code;
    size_t count;
    size_t capacity;
    uint32_t *lines;
} SnChunk;

typedef struct {
    char **strings;
    size_t count;
    size_t capacity;
} SnStringPool;

typedef struct {
    const char *name;
    uint32_t arity;
    uint32_t local_count;
    SnChunk chunk;
} SnFunctionChunk;

typedef struct {
    SnStringPool string_pool;
    SnFunctionChunk **functions;
    size_t function_count;
    size_t function_capacity;
    uint32_t main_func_idx;
} SnBCUnit;

void sn_chunk_init(SnChunk *chunk);
void sn_chunk_free(SnChunk *chunk);
void sn_chunk_write(SnChunk *chunk, uint8_t byte, uint32_t line);
void sn_chunk_write_u32(SnChunk *chunk, uint32_t val, uint32_t line);
void sn_chunk_write_i64(SnChunk *chunk, int64_t val, uint32_t line);
void sn_chunk_write_double(SnChunk *chunk, double val, uint32_t line);

void sn_bcunit_init(SnBCUnit *unit);
void sn_bcunit_free(SnBCUnit *unit);
uint32_t sn_bcunit_add_string(SnBCUnit *unit, const char *str);
uint32_t sn_bcunit_add_function(SnBCUnit *unit, const char *name, uint32_t arity);

#endif /* SNOVAC_SNBC_H */
