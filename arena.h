/* arena.h — bump allocator.
 *
 * Every AST node, token vector and interned string lives in an arena and is
 * freed in one shot by sn_arena_free(). Nothing in snovac frees an individual
 * node, so there is no ownership bookkeeping in the front-end at all.
 */
#ifndef SNOVAC_ARENA_H
#define SNOVAC_ARENA_H

#include <stddef.h>

typedef struct SnArenaBlock SnArenaBlock;

typedef struct {
    SnArenaBlock *head;
    size_t block_size;
    size_t total_bytes;
} SnArena;

/* Allocation failure is fatal: it aborts with a message instead of returning
 * NULL. A compiler that cannot allocate cannot produce a correct answer, and
 * making every call site NULL-check invites the far worse failure mode of
 * silently dropping a token or an AST node and carrying on. */
void  sn_arena_init(SnArena *a, size_t block_size);
void *sn_arena_alloc(SnArena *a, size_t size);
void *sn_arena_calloc(SnArena *a, size_t size);
char *sn_arena_strndup(SnArena *a, const char *s, size_t n);
void  sn_arena_free(SnArena *a);

#endif /* SNOVAC_ARENA_H */
