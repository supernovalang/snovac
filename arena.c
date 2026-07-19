#include "arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SN_ARENA_ALIGN 16u

struct SnArenaBlock {
    SnArenaBlock *next;
    size_t used;
    size_t cap;
    unsigned char data[];
};

static size_t align_up(size_t n) {
    return (n + (SN_ARENA_ALIGN - 1u)) & ~((size_t)SN_ARENA_ALIGN - 1u);
}

void sn_arena_init(SnArena *a, size_t block_size) {
    a->head = NULL;
    a->block_size = block_size ? block_size : (size_t)(64 * 1024);
    a->total_bytes = 0;
}

static void oom(size_t size) {
    fprintf(stderr, "snovac: fatal: out of memory allocating %zu bytes\n", size);
    exit(70); /* EX_SOFTWARE */
}

static SnArenaBlock *push_block(SnArena *a, size_t need) {
    size_t cap = a->block_size;
    if (cap < need) {
        cap = align_up(need);
    }
    SnArenaBlock *b = (SnArenaBlock *)malloc(sizeof(SnArenaBlock) + cap);
    if (!b) {
        oom(cap);
    }
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    a->total_bytes += cap;
    return b;
}

void *sn_arena_alloc(SnArena *a, size_t size) {
    size_t need = align_up(size ? size : 1u);
    SnArenaBlock *b = a->head;
    if (!b || b->cap - b->used < need) {
        b = push_block(a, need);
    }
    void *p = b->data + b->used;
    b->used += need;
    return p;
}

void *sn_arena_calloc(SnArena *a, size_t size) {
    void *p = sn_arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

char *sn_arena_strndup(SnArena *a, const char *s, size_t n) {
    char *p = (char *)sn_arena_alloc(a, n + 1u);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void sn_arena_free(SnArena *a) {
    SnArenaBlock *b = a->head;
    while (b) {
        SnArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->total_bytes = 0;
}
