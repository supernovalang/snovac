/* package.h — package graph: discovery, header parsing, import cycles.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §4.1 / §8 step 4.
 *
 * A package is identified by its `package a.b.c` declaration, never by
 * directory layout — one package can span many files and directories (the
 * corpus depends on this: `compiler.lsp` alone spans 37 files under
 * compiler/src/lsp/). Discovery therefore recursively walks the given
 * root(s), header-parses every `*.snova` file (package + import lines only,
 * not the full grammar), and groups files by declared package name.
 *
 * Import edges are between PACKAGES, not files: an import cycle across two
 * or more distinct packages is an error (SNOVA0108), listing the full cycle.
 * A cycle between files of the SAME package is legal — it is one package —
 * so same-package imports never become graph edges at all.
 *
 * What this module deliberately does NOT do (out of scope for P2.2, owned by
 * P2.3/P2.4 resolve.c instead):
 *   - Deciding whether an import target that isn't a discovered package name
 *     might still be a valid *native* package under some other naming
 *     convention. The compile-fail fixtures `packages/console_without_import`
 *     and `packages/unknown_package` (SNOVA017) reference `Snovalang.Console`
 *     / `Snovalang.LegacyDatabase`, a capitalized prefix that does not match
 *     any `package` declaration actually found under `builtin` today (those
 *     declare `builtin.console.Console`, `builtin.filesystem.FileSystem`,
 *     etc., lowercase). This is a real naming mismatch between
 *     those two fixtures and the current builtin/ layout — flagged, not
 *     resolved, here. Only SNOVA050 ("import target does not exist as any
 *     discovered package") is handled by this module; SNOVA017 is left for
 *     whoever designs native-package resolution in P2.3.
 */
#ifndef SNOVAC_PACKAGE_H
#define SNOVAC_PACKAGE_H

#include <stddef.h>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"

/* Package-graph band, carved out of the shared 0100-0199 range (see
 * parse_internal.h for why 100/101 are off-limits). SNOVA050 is reused as-is
 * from tests/compile-fail/missing_import — it predates this module and its
 * meaning ("imported package was not found") already matches exactly. */
#define SNOVA_IMPORT_CYCLE          108
#define SNOVA_MISSING_PACKAGE_DECL  109
#define SNOVA_IMPORT_NOT_FOUND      50

typedef struct SnPackageFile SnPackageFile;
struct SnPackageFile {
    const char *path;    /* arena-owned, as passed to the scanner */
    const char *src;     /* arena-owned file text; spans below index into it */
    size_t src_len;
    const char *package; /* interned; NULL if the file had no `package` line */
    SnSpan package_span;  /* span of the package's qualified name, for diagnostics */
    SnList imports;       /* const char* — interned qualified names */
    SnList import_spans;  /* SnSpan* — parallel to `imports` */
    SnPackageFile *next;  /* next file of the same SnPackageNode */
};

typedef struct SnPackageNode SnPackageNode;
struct SnPackageNode {
    const char *name; /* interned dotted package name */
    SnPackageFile *files;
    SnList edges; /* const char* — interned target package names, deduped,
                   * cross-package only (same-package imports are dropped) */
    SnPackageNode *next; /* graph-wide node list */

    /* cycle-detection scratch state (sn_pkggraph_find_cycle), reset before
     * each detection run */
    uint8_t on_stack;
    uint8_t visited;
};

typedef struct {
    SnArena *arena;
    SnInternTable *intern;
    SnDiagSink *diag;
    SnPackageNode *nodes;
    size_t node_count;
    size_t file_count;

    /* Native-package manifest (sn_pkggraph_load_native_manifest) — interned
     * qualified names of "builtin." / "stdlib." packages the toolchain
     * supplies with no discoverable .snova file (see package.c's
     * native-package comment). native_manifest_loaded distinguishes
     * "loaded, and this name isn't in it" (reject) from "never found a
     * manifest to load" (no signal, fall back to the old permissive rule). */
    SnList native_manifest;
    int native_manifest_loaded;
} SnPackageGraph;

void sn_pkggraph_init(SnPackageGraph *g, SnArena *a, SnInternTable *it,
                       SnDiagSink *diag);

/* Recursively scans `root` for `*.snova` files, header-parses each, and adds
 * every file to the graph, creating or reusing SnPackageNode entries by
 * declared package name. A file that fails to lex, or that has no `package`
 * line, is reported through `diag` (SNOVA_MISSING_PACKAGE_DECL for the
 * latter) and excluded from the graph. Returns the number of `*.snova` files
 * found under `root` (including excluded ones). */
size_t sn_pkggraph_scan_root(SnPackageGraph *g, const char *root);

/* Scans exactly one file (not a directory) — for callers that want ONE
 * file's own declarations without absorbing every sibling under the same
 * directory (sn_pkggraph_scan_root() would). Returns 1 on success, 0 if the
 * file couldn't be opened. */
int sn_pkggraph_scan_single_file(SnPackageGraph *g, const char *path);

/* Loads `<builtin_dir>/native-packages.list` (generated by
 * scripts/gen-packages.sh from compiler/src/lsp/NativePackages.snova) into
 * `g->native_manifest`, interning each line. Missing file is not an error —
 * it just leaves `native_manifest_loaded` at 0, so sn_pkggraph_link() falls
 * back to the pre-manifest permissive rule for "builtin." / "stdlib."
 * imports. Call once, after scanning `builtin_dir` as a root and before
 * sn_pkggraph_link(). */
void sn_pkggraph_load_native_manifest(SnPackageGraph *g, const char *builtin_dir);

/* Resolves every file's imports into cross-package edges now that all roots
 * have been scanned. An import naming a package absent from the graph is
 * reported as SNOVA_IMPORT_NOT_FOUND at the import's span. Call once, after
 * all sn_pkggraph_scan_root() calls. */
void sn_pkggraph_link(SnPackageGraph *g);

SnPackageNode *sn_pkggraph_find(const SnPackageGraph *g, const char *name);

/* Depth-first search for one cycle reachable from any node. Returns 1 and
 * fills `*cycle_out` with the interned package names in cycle order
 * (`cycle_out` items[0] repeated at the end, so the array reads as a closed
 * loop) when a cycle exists; returns 0 and leaves `*cycle_out` untouched
 * otherwise. Does not itself emit a diagnostic — callers decide the span
 * (there is no single "right" file for a package-level cycle). */
int sn_pkggraph_find_cycle(SnPackageGraph *g, SnList *cycle_out);

#endif /* SNOVAC_PACKAGE_H */
