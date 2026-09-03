#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "snbc.h"

#include <stdlib.h>
#include <string.h>

void sn_chunk_init(SnChunk *chunk) {
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
}

void sn_chunk_free(SnChunk *chunk) {
    free(chunk->code);
    free(chunk->lines);
    sn_chunk_init(chunk);
}

void sn_chunk_write(SnChunk *chunk, uint8_t byte, uint32_t line) {
    if (chunk->capacity < chunk->count + 1) {
        size_t old_cap = chunk->capacity;
        chunk->capacity = old_cap < 8 ? 8 : old_cap * 2;
        chunk->code = (uint8_t *)realloc(chunk->code, chunk->capacity);
        chunk->lines = (uint32_t *)realloc(chunk->lines, chunk->capacity * sizeof(uint32_t));
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void sn_chunk_write_u32(SnChunk *chunk, uint32_t val, uint32_t line) {
    for (int i = 0; i < 4; i++) {
        sn_chunk_write(chunk, (uint8_t)((val >> (i * 8)) & 0xFF), line);
    }
}

void sn_chunk_write_i64(SnChunk *chunk, int64_t val, uint32_t line) {
    uint64_t u = (uint64_t)val;
    for (int i = 0; i < 8; i++) {
        sn_chunk_write(chunk, (uint8_t)((u >> (i * 8)) & 0xFF), line);
    }
}

void sn_chunk_write_double(SnChunk *chunk, double val, uint32_t line) {
    union {
        double d;
        uint64_t u;
    } cvt;
    cvt.d = val;
    for (int i = 0; i < 8; i++) {
        sn_chunk_write(chunk, (uint8_t)((cvt.u >> (i * 8)) & 0xFF), line);
    }
}

void sn_bcunit_init(SnBCUnit *unit) {
    unit->string_pool.strings = NULL;
    unit->string_pool.count = 0;
    unit->string_pool.capacity = 0;
    unit->functions = NULL;
    unit->function_count = 0;
    unit->function_capacity = 0;
    unit->main_func_idx = 0;
}

void sn_bcunit_free(SnBCUnit *unit) {
    for (size_t i = 0; i < unit->string_pool.count; i++) {
        free(unit->string_pool.strings[i]);
    }
    free(unit->string_pool.strings);
    for (size_t i = 0; i < unit->function_count; i++) {
        sn_chunk_free(&unit->functions[i]->chunk);
        free(unit->functions[i]);
    }
    free(unit->functions);
    sn_bcunit_init(unit);
}

uint32_t sn_bcunit_add_string(SnBCUnit *unit, const char *str) {
    for (size_t i = 0; i < unit->string_pool.count; i++) {
        if (strcmp(unit->string_pool.strings[i], str) == 0) {
            return (uint32_t)i;
        }
    }
    if (unit->string_pool.capacity < unit->string_pool.count + 1) {
        size_t old_cap = unit->string_pool.capacity;
        unit->string_pool.capacity = old_cap < 8 ? 8 : old_cap * 2;
        unit->string_pool.strings = (char **)realloc(unit->string_pool.strings,
                                                    unit->string_pool.capacity * sizeof(char *));
    }
    uint32_t idx = (uint32_t)unit->string_pool.count;
    unit->string_pool.strings[idx] = strdup(str);
    unit->string_pool.count++;
    return idx;
}

uint32_t sn_bcunit_add_function(SnBCUnit *unit, const char *name, uint32_t arity) {
    if (unit->function_capacity < unit->function_count + 1) {
        size_t old_cap = unit->function_capacity;
        unit->function_capacity = old_cap < 8 ? 8 : old_cap * 2;
        unit->functions = (SnFunctionChunk **)realloc(unit->functions,
                                                     unit->function_capacity * sizeof(SnFunctionChunk *));
    }
    SnFunctionChunk *fn = (SnFunctionChunk *)malloc(sizeof(SnFunctionChunk));
    fn->name = name;
    fn->arity = arity;
    fn->local_count = arity;
    sn_chunk_init(&fn->chunk);

    uint32_t idx = (uint32_t)unit->function_count;
    unit->functions[idx] = fn;
    unit->function_count++;
    return idx;
}
