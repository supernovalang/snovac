/* test_symbol.c — nested-scope lookup assertions for intern.c / symbol.c.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 step 3 asks for
 * "unit tests de escopo aninhado". intern.c and symbol.c have no CLI surface
 * of their own yet (that arrives with resolve.c, several steps later), so —
 * unlike tests/run.sh, which drives the lexer/parser through the snovac
 * binary — this is a standalone C program that links the two modules
 * directly and asserts on their behavior.
 */
#include <stdio.h>
#include <string.h>

#include "../arena.h"
#include "../intern.h"
#include "../symbol.h"

static int pass = 0;
static int fail = 0;

#define CHECK(name, cond)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            pass++;                                                         \
        } else {                                                             \
            fail++;                                                         \
            printf("FAIL %s\n", name);                                      \
        }                                                                    \
    } while (0)

static void test_intern(SnInternTable *it) {
    const char *a1 = sn_intern_cstr(it, "hello");
    const char *a2 = sn_intern_cstr(it, "hello");
    const char *b = sn_intern_cstr(it, "world");
    CHECK("intern: equal strings intern to the same pointer", a1 == a2);
    CHECK("intern: different strings intern to different pointers", a1 != b);
    CHECK("intern: content preserved", strcmp(a1, "hello") == 0);

    /* len-bounded variant must agree with the NUL-terminated one */
    const char *sliced = sn_intern(it, "helloXXXX", 5);
    CHECK("intern: sn_intern respects len, not the NUL", sliced == a1);

    /* two distinct strings that happen to share a prefix must not collide */
    const char *he = sn_intern_cstr(it, "he");
    const char *hell = sn_intern_cstr(it, "hell");
    CHECK("intern: prefix of an interned string is a distinct entry",
          he != a1 && hell != a1 && he != hell);

    /* force several rehashes and confirm early entries are still found */
    char buf[32];
    const char *first = NULL;
    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        const char *p = sn_intern_cstr(it, buf);
        if (i == 0) {
            first = p;
        }
    }
    const char *refound = sn_intern_cstr(it, "sym_0");
    CHECK("intern: survives rehash", refound == first);
}

static void test_scope_single(SnInternTable *it, SnArena *a) {
    SnScope root;
    sn_scope_init(&root, a, NULL);

    const char *foo = sn_intern_cstr(it, "foo");
    SnSpan span0;
    memset(&span0, 0, sizeof(span0));

    SnSymbol *def1 = sn_scope_define(&root, foo, SN_SYM_LOCAL, NULL, span0);
    CHECK("scope: first define succeeds", def1 != NULL);
    CHECK("scope: defined symbol keeps its name/kind", def1 != NULL &&
                                                            def1->name == foo &&
                                                            def1->kind == SN_SYM_LOCAL);

    SnSymbol *dup = sn_scope_define(&root, foo, SN_SYM_LOCAL, NULL, span0);
    CHECK("scope: duplicate define in the same scope is rejected", dup == NULL);
    CHECK("scope: rejected duplicate did not replace the original",
          sn_scope_lookup_local(&root, foo) == def1);

    CHECK("scope: local lookup finds it", sn_scope_lookup_local(&root, foo) == def1);

    const char *bar = sn_intern_cstr(it, "bar");
    CHECK("scope: undefined name is not found",
          sn_scope_lookup_local(&root, bar) == NULL);
    CHECK("scope: lookup (not just lookup_local) also fails for undefined names",
          sn_scope_lookup(&root, bar) == NULL);
}

static void test_scope_nesting(SnInternTable *it, SnArena *a) {
    SnSpan span0;
    memset(&span0, 0, sizeof(span0));

    SnScope root;
    sn_scope_init(&root, a, NULL);
    const char *foo = sn_intern_cstr(it, "foo");
    SnSymbol *def1 = sn_scope_define(&root, foo, SN_SYM_LOCAL, NULL, span0);

    SnScope child;
    sn_scope_init(&child, a, &root);
    CHECK("scope: child sees parent's symbol via lookup",
          sn_scope_lookup(&child, foo) == def1);
    CHECK("scope: child lookup_local does NOT see the parent's symbol",
          sn_scope_lookup_local(&child, foo) == NULL);

    SnSymbol *shadow = sn_scope_define(&child, foo, SN_SYM_LOCAL, NULL, span0);
    CHECK("scope: child can shadow the parent's name",
          shadow != NULL && shadow != def1);
    CHECK("scope: lookup from the child now finds the shadow",
          sn_scope_lookup(&child, foo) == shadow);
    CHECK("scope: the parent itself is unaffected by the child's shadow",
          sn_scope_lookup_local(&root, foo) == def1);

    SnScope grandchild;
    sn_scope_init(&grandchild, a, &child);
    const char *baz = sn_intern_cstr(it, "baz_not_yet_defined");
    CHECK("scope: grandchild walks multiple levels and still fails cleanly",
          sn_scope_lookup(&grandchild, baz) == NULL);

    SnSymbol *def_baz = sn_scope_define(&root, baz, SN_SYM_TYPE, NULL, span0);
    CHECK("scope: grandchild sees a symbol added later to a distant ancestor",
          sn_scope_lookup(&grandchild, baz) == def_baz);
}

static void test_scope_rehash(SnInternTable *it, SnArena *a) {
    SnSpan span0;
    memset(&span0, 0, sizeof(span0));

    SnScope big;
    sn_scope_init(&big, a, NULL);

    char buf[32];
    SnSymbol *first_sym = NULL;
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "member_%d", i);
        const char *name = sn_intern_cstr(it, buf);
        SnSymbol *sym = sn_scope_define(&big, name, SN_SYM_METHOD, NULL, span0);
        if (i == 0) {
            first_sym = sym;
        }
    }
    const char *member0 = sn_intern_cstr(it, "member_0");
    const char *member199 = sn_intern_cstr(it, "member_199");
    CHECK("scope: survives rehash — earliest entry still found",
          sn_scope_lookup_local(&big, member0) == first_sym);
    CHECK("scope: survives rehash — latest entry also found",
          sn_scope_lookup_local(&big, member199) != NULL);
}

int main(void) {
    SnArena arena;
    sn_arena_init(&arena, 0);

    SnInternTable it;
    sn_intern_init(&it, &arena);

    test_intern(&it);
    test_scope_single(&it, &arena);
    test_scope_nesting(&it, &arena);
    test_scope_rehash(&it, &arena);

    sn_arena_free(&arena);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
