#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "snbc.h"

#include <stdio.h>
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
    fn->name = name ? strdup(name) : NULL;
    fn->arity = arity;
    fn->local_count = arity;
    sn_chunk_init(&fn->chunk);

    uint32_t idx = (uint32_t)unit->function_count;
    unit->functions[idx] = fn;
    unit->function_count++;
    return idx;
}

int sn_bcunit_write_file(const SnBCUnit *unit, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    uint32_t magic = SNBC_MAGIC;
    uint32_t version = SNBC_VERSION;
    uint32_t main_idx = unit->main_func_idx;

    fwrite(&magic, sizeof(uint32_t), 1, f);
    fwrite(&version, sizeof(uint32_t), 1, f);
    fwrite(&main_idx, sizeof(uint32_t), 1, f);

    uint32_t str_count = (uint32_t)unit->string_pool.count;
    fwrite(&str_count, sizeof(uint32_t), 1, f);
    for (uint32_t i = 0; i < str_count; i++) {
        const char *s = unit->string_pool.strings[i];
        uint32_t slen = (uint32_t)(s ? strlen(s) : 0);
        fwrite(&slen, sizeof(uint32_t), 1, f);
        if (slen > 0) {
            fwrite(s, 1, slen, f);
        }
    }

    uint32_t fn_count = (uint32_t)unit->function_count;
    fwrite(&fn_count, sizeof(uint32_t), 1, f);
    for (uint32_t i = 0; i < fn_count; i++) {
        SnFunctionChunk *fn = unit->functions[i];
        uint32_t nlen = (uint32_t)(fn->name ? strlen(fn->name) : 0);
        fwrite(&nlen, sizeof(uint32_t), 1, f);
        if (nlen > 0) {
            fwrite(fn->name, 1, nlen, f);
        }
        fwrite(&fn->arity, sizeof(uint32_t), 1, f);
        fwrite(&fn->local_count, sizeof(uint32_t), 1, f);
        uint32_t code_len = (uint32_t)fn->chunk.count;
        fwrite(&code_len, sizeof(uint32_t), 1, f);
        if (code_len > 0) {
            fwrite(fn->chunk.code, 1, code_len, f);
        }
    }

    fclose(f);
    return 1;
}

int sn_bcunit_read_file(SnBCUnit *unit, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic = 0, version = 0, main_idx = 0;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 || magic != SNBC_MAGIC) {
        fclose(f);
        return 0;
    }
    if (fread(&version, sizeof(uint32_t), 1, f) != 1 || version != SNBC_VERSION) {
        fclose(f);
        return 0;
    }
    if (fread(&main_idx, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    unit->main_func_idx = main_idx;

    uint32_t str_count = 0;
    if (fread(&str_count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    for (uint32_t i = 0; i < str_count; i++) {
        uint32_t slen = 0;
        if (fread(&slen, sizeof(uint32_t), 1, f) != 1) {
            fclose(f);
            return 0;
        }
        char *buf = (char *)malloc(slen + 1);
        if (slen > 0) {
            if (fread(buf, 1, slen, f) != slen) {
                free(buf);
                fclose(f);
                return 0;
            }
        }
        buf[slen] = '\0';
        sn_bcunit_add_string(unit, buf);
        free(buf);
    }

    uint32_t fn_count = 0;
    if (fread(&fn_count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    for (uint32_t i = 0; i < fn_count; i++) {
        uint32_t nlen = 0;
        if (fread(&nlen, sizeof(uint32_t), 1, f) != 1) {
            fclose(f);
            return 0;
        }
        char *name = NULL;
        if (nlen > 0) {
            name = (char *)malloc(nlen + 1);
            if (fread(name, 1, nlen, f) != nlen) {
                free(name);
                fclose(f);
                return 0;
            }
            name[nlen] = '\0';
        }
        uint32_t arity = 0, local_count = 0, code_len = 0;
        if (fread(&arity, sizeof(uint32_t), 1, f) != 1 ||
            fread(&local_count, sizeof(uint32_t), 1, f) != 1 ||
            fread(&code_len, sizeof(uint32_t), 1, f) != 1) {
            free(name);
            fclose(f);
            return 0;
        }
        uint32_t fidx = sn_bcunit_add_function(unit, name, arity);
        unit->functions[fidx]->local_count = local_count;
        if (code_len > 0) {
            uint8_t *code = (uint8_t *)malloc(code_len);
            if (fread(code, 1, code_len, f) != code_len) {
                free(code);
                free(name);
                fclose(f);
                return 0;
            }
            for (uint32_t ci = 0; ci < code_len; ci++) {
                sn_chunk_write(&unit->functions[fidx]->chunk, code[ci], 1);
            }
            free(code);
        }
        free(name);
    }

    fclose(f);
    return 1;
}

int sn_bcunit_merge(SnBCUnit *dest, const SnBCUnit *src) {
    if (!dest || !src) return 0;
    for (size_t i = 0; i < src->function_count; i++) {
        SnFunctionChunk *sfn = src->functions[i];
        uint32_t fidx = sn_bcunit_add_function(dest, sfn->name, sfn->arity);
        dest->functions[fidx]->local_count = sfn->local_count;
        for (size_t ci = 0; ci < sfn->chunk.count; ci++) {
            sn_chunk_write(&dest->functions[fidx]->chunk, sfn->chunk.code[ci], 1);
        }
    }
    return 1;
}
