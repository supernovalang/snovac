/* test_package.c — package.c: discovery, linking, cycle detection.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 step 4 verification
 * ("corpus inteiro mapeado sem erro; ciclo sintético detectado"). Two parts:
 *
 *   1. Synthetic fixtures written to a scratch directory under the OS tmp
 *      dir, covering discovery, multi-file packages, missing imports, real
 *      cycles, legal same-package "cycles", multi-section files (parse.c's
 *      documented precedent — a file may declare more than one `package`),
 *      and files missing a `package` line entirely.
 *
 *   2. A report-only pass over the real corpus (compiler/src/ + builtin/).
 *      This intentionally does NOT assert zero link errors: it currently
 *      finds 6 real package-name mismatches inside compiler/src/ (e.g.
 *      `import builtin.metadata.Documented` vs the file's actual
 *      `package builtin.metadata`; `import snovalang.compiler.hir` vs
 *      `package compiler.hir`). Every source file under compiler/src is
 *      READ-ONLY for this spec (plan.md §0 — it's the self-hosted compiler's
 *      design reference, a different module/skill), so this test cannot fix them
 *      and must not launder them into a false "0 errors" by loosening
 *      package.c. It only hard-asserts on file/node counts and on the
 *      absence of an import cycle, which is a real invariant, not a
 *      workaround for someone else's typo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../arena.h"
#include "../diag.h"
#include "../intern.h"
#include "../package.h"

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

/* ── scratch fixture helpers ─────────────────────────────────────────────── */

static void write_file(const char *dir, const char *name, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("FAIL setup: could not create %s\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

/* Fixtures are written under the OS tmp dir (mkdtemp) and deliberately left
 * in place rather than cleaned up here — they're a few hundred bytes each,
 * and adding a directory walker just to unlink them isn't worth it in a test
 * binary. */

/* ── individual scenarios ────────────────────────────────────────────────── */

static void test_basic_discovery(SnInternTable *it, SnArena *a, const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/basic", tmp);
    mkdir(dir, 0755);
    write_file(dir, "a1.snova", "package pkg.a\nimport pkg.b\n");
    write_file(dir, "a2.snova", "package pkg.a\n");
    write_file(dir, "b.snova", "package pkg.b\n");

    SnDiagSink diag;
    sn_diag_init(&diag, "<test>", "", 0);
    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);

    size_t found = sn_pkggraph_scan_root(&g, dir);
    CHECK("basic: finds all 3 files", found == 3);
    CHECK("basic: groups into 2 packages", g.node_count == 2);
    CHECK("basic: pkg.a has 2 files", g.file_count == 3);

    SnPackageNode *a_node = sn_pkggraph_find(&g, sn_intern_cstr(it, "pkg.a"));
    SnPackageNode *b_node = sn_pkggraph_find(&g, sn_intern_cstr(it, "pkg.b"));
    CHECK("basic: pkg.a found", a_node != NULL);
    CHECK("basic: pkg.b found", b_node != NULL);

    int a_file_count = 0;
    if (a_node) {
        for (SnPackageFile *f = a_node->files; f; f = f->next) {
            a_file_count++;
        }
    }
    CHECK("basic: pkg.a groups exactly its 2 files", a_file_count == 2);

    sn_pkggraph_link(&g);
    CHECK("basic: no link errors", diag.error_count == 0);
    CHECK("basic: pkg.a has one edge to pkg.b",
          a_node != NULL && a_node->edges.len == 1 &&
              SN_LIST_AT(a_node->edges, const char, 0) == b_node->name);

    SnList cycle;
    memset(&cycle, 0, sizeof(cycle));
    CHECK("basic: no cycle", sn_pkggraph_find_cycle(&g, &cycle) == 0);
}

static void test_missing_import_target(SnInternTable *it, SnArena *a, const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/missing", tmp);
    mkdir(dir, 0755);
    write_file(dir, "x.snova", "package pkg.x\nimport pkg.does_not_exist\n");

    SnDiagSink diag;
    sn_diag_init(&diag, "<test>", "", 0);
    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    sn_pkggraph_scan_root(&g, dir);
    sn_pkggraph_link(&g);

    CHECK("missing-import: reports exactly one error", diag.error_count == 1);
}

static void test_cycle_detection(SnInternTable *it, SnArena *a, const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/cycle", tmp);
    mkdir(dir, 0755);
    write_file(dir, "c1.snova", "package pkg.c1\nimport pkg.c2\n");
    write_file(dir, "c2.snova", "package pkg.c2\nimport pkg.c3\n");
    write_file(dir, "c3.snova", "package pkg.c3\nimport pkg.c1\n");

    SnDiagSink diag;
    sn_diag_init(&diag, "<test>", "", 0);
    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    sn_pkggraph_scan_root(&g, dir);
    sn_pkggraph_link(&g);
    CHECK("cycle: linking itself reports no errors (all targets exist)",
          diag.error_count == 0);

    SnList cycle;
    memset(&cycle, 0, sizeof(cycle));
    int found = sn_pkggraph_find_cycle(&g, &cycle);
    CHECK("cycle: a 3-package cycle is detected", found == 1);

    /* The DFS may start from any of the three nodes depending on graph node
     * order, so check the SET of names involved rather than one fixed
     * rotation; a closed 3-cycle reported as a path is 4 entries long
     * (start repeated at the end). */
    CHECK("cycle: reported path has 4 entries (3-cycle, closed)",
          cycle.len == 4);
    if (cycle.len == 4) {
        const char *n0 = SN_LIST_AT(cycle, const char, 0);
        const char *n3 = SN_LIST_AT(cycle, const char, 3);
        CHECK("cycle: path closes on itself (first == last)", n0 == n3);

        int has_c1 = 0, has_c2 = 0, has_c3 = 0;
        for (size_t i = 0; i < 3; i++) {
            const char *name = SN_LIST_AT(cycle, const char, i);
            if (name == sn_intern_cstr(it, "pkg.c1")) has_c1 = 1;
            if (name == sn_intern_cstr(it, "pkg.c2")) has_c2 = 1;
            if (name == sn_intern_cstr(it, "pkg.c3")) has_c3 = 1;
        }
        CHECK("cycle: all three packages are on the reported cycle",
              has_c1 && has_c2 && has_c3);
    }
}

static void test_same_package_self_import_is_legal(SnInternTable *it, SnArena *a,
                                                    const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/selfimport", tmp);
    mkdir(dir, 0755);
    write_file(dir, "d1.snova", "package pkg.d\nimport pkg.d\n");
    write_file(dir, "d2.snova", "package pkg.d\n");

    SnDiagSink diag;
    sn_diag_init(&diag, "<test>", "", 0);
    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    sn_pkggraph_scan_root(&g, dir);
    sn_pkggraph_link(&g);

    SnPackageNode *d = sn_pkggraph_find(&g, sn_intern_cstr(it, "pkg.d"));
    CHECK("self-import: package found", d != NULL);
    CHECK("self-import: no error (target exists — it's itself)",
          diag.error_count == 0);
    CHECK("self-import: produces no edge (same-package import is not a graph edge)",
          d != NULL && d->edges.len == 0);

    SnList cycle;
    memset(&cycle, 0, sizeof(cycle));
    CHECK("self-import: not reported as a cycle",
          sn_pkggraph_find_cycle(&g, &cycle) == 0);
}

static void test_multi_section_file(SnInternTable *it, SnArena *a, const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/multisection", tmp);
    mkdir(dir, 0755);
    /* Mirrors tests/compile-fail/visibility_internal_cross_package.snova:
     * two `package` sections in one physical file. */
    write_file(dir, "e.snova",
               "package pkg.e1\n\nfunc providerFn(): int { return 1 }\n\n"
               "package pkg.e2\n\nfunc consumerFn(): int { return providerFn() }\n");

    SnDiagSink diag;
    sn_diag_init(&diag, "<test>", "", 0);
    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    size_t found = sn_pkggraph_scan_root(&g, dir);

    CHECK("multi-section: scan_root still counts it as 1 physical file",
          found == 1);
    CHECK("multi-section: both sections become separate package nodes",
          g.node_count == 2);
    CHECK("multi-section: total section-files recorded is 2",
          g.file_count == 2);
    CHECK("multi-section: pkg.e1 discovered",
          sn_pkggraph_find(&g, sn_intern_cstr(it, "pkg.e1")) != NULL);
    CHECK("multi-section: pkg.e2 discovered",
          sn_pkggraph_find(&g, sn_intern_cstr(it, "pkg.e2")) != NULL);
}

static void test_missing_package_decl(SnInternTable *it, SnArena *a, const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/nopkg", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova", "import pkg.whatever\n");

    SnDiagSink diag;
    sn_diag_init(&diag, "<test>", "", 0);
    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    sn_pkggraph_scan_root(&g, dir);

    CHECK("no-package: reports exactly one diagnostic", diag.error_count == 1);
    CHECK("no-package: contributes no package node", g.node_count == 0);
    CHECK("no-package: contributes no file", g.file_count == 0);
}

/* ── real corpus (report-only, see the file header for why) ────────────── */

static void report_real_corpus(SnInternTable *it, SnArena *a) {
    SnDiagSink diag;
    sn_diag_init(&diag, "<corpus-scan>", "", 0);
    diag.out = fopen("/dev/null", "w"); /* the point here is the counters, not the text */
    if (!diag.out) {
        diag.out = stderr;
    }

    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    size_t f1 = sn_pkggraph_scan_root(&g, "../compiler/src");
    size_t f2 = sn_pkggraph_scan_root(&g, "../builtin");
    sn_pkggraph_link(&g);

    SnList cycle;
    memset(&cycle, 0, sizeof(cycle));
    int has_cycle = sn_pkggraph_find_cycle(&g, &cycle);

    printf("\n[real corpus] compiler/src+builtin: %zu+%zu=%zu files scanned, "
           "%zu packages, %zu link errors (known pre-existing corpus "
           "mismatches, out of snovac's module scope — see file header), "
           "cycle=%d\n",
           f1, f2, f1 + f2, g.node_count, (size_t)diag.error_count, has_cycle);

    CHECK("real corpus: no import cycle across compiler/src+builtin",
          has_cycle == 0);

    if (diag.out != stderr) {
        fclose(diag.out);
    }
}

int main(void) {
    SnArena arena;
    sn_arena_init(&arena, 0);
    SnInternTable it;
    sn_intern_init(&it, &arena);

    char tmp[] = "/tmp/snovac_pkg_test_XXXXXX";
    if (!mkdtemp(tmp)) {
        printf("FAIL setup: mkdtemp failed\n");
        return 1;
    }

    test_basic_discovery(&it, &arena, tmp);
    test_missing_import_target(&it, &arena, tmp);
    test_cycle_detection(&it, &arena, tmp);
    test_same_package_self_import_is_legal(&it, &arena, tmp);
    test_multi_section_file(&it, &arena, tmp);
    test_missing_package_decl(&it, &arena, tmp);
    report_real_corpus(&it, &arena);

    sn_arena_free(&arena);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
