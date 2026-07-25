/* resolve.h — declaration collection + name resolution + prelude.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §4.2/§4.3, §8 step 6.
 * Runs after package.c (package graph, header-only) and needs a real parse
 * of every file — package.c never builds an AST, only package/import
 * headers, so this module re-reads and fully parses each file.
 *
 * ── Known, documented scope boundaries (not oversights) ────────────────────
 *
 * 1. Multi-section files are skipped. parse.c documents that a file may
 *    contain more than one `package` section (tests/compile-fail/
 *    visibility_internal_cross_package.snova), and package.c (P2.2) already
 *    splits those into separate SnPackageFile entries per section. But
 *    sn_parse()/SnUnit do NOT track section boundaries — every decl in the
 *    file lands in one flat `SnUnit.decls`, with no marker for which
 *    section it came from. Correctly attributing declarations per section
 *    needs a parser/AST change, which is out of scope here (a P1-gated file
 *    change, decided against for this pass — see llm.md). sn_resolver_collect()
 *    detects these files (via package.c already having produced more than
 *    one SnPackageFile for the same path) and skips full declaration
 *    collection for them entirely, returning the skipped-file count so
 *    callers can report it. As of 2026-07-25 this affects exactly one
 *    fixture.
 *
 * 2. Primitive types (`int`, `string`, `bool`, `unit`, `long`, `double`,
 *    `decimal`) are NOT registered as symbols. Contrary to plan.md's fact 2
 *    ("primitives are declared in builtin.types"), no `.snova` file anywhere
 *    in this repository actually declares them (measured 2026-07-25 — zero
 *    `class`/`struct`/`typealias` declarations for any of the seven names).
 *    types.c already models them as compiler-intrinsic tags with no `decl`.
 *    sn_resolve_type_name() special-cases these seven names directly against
 *    types.c's singletons, before any scope lookup — confirmed with the user
 *    before implementing (this is a resolver design choice, not silently
 *    inferred).
 *
 * 3. `Option`/`Result` come from one specific, hardcoded package name:
 *    `builtin.types.Types` (matches the `package builtin.<lower(Stem)>.<Stem>`
 *    convention documented elsewhere in this repo's specs). If that package
 *    is ever renamed, sn_resolver_build_prelude() silently finds nothing —
 *    it does not search the whole graph for a package that happens to
 *    declare Option/Result. Their VARIANTS (`Some`/`None`/`Ok`/`Err`) are
 *    also added to the prelude scope directly, not just the two type names
 *    — builtin/Types.snova's own doc comment says construction is "usable
 *    in every Snovalang program with no import"; found this the hard way
 *    when `Some(1)` failed to resolve during check.c testing.
 *
 * 4. A FIELD and a METHOD with the same name in one type do NOT collide
 *    (SNOVA_DUPLICATE_DECL is not raised) — `private let path: Path` plus
 *    `method path(): Path` both exist for real in builtin/FileSystem.snova's
 *    `File`, an idiomatic backing-field/accessor pair. Whichever is
 *    collected first wins the name; disambiguating "the field" from "a call
 *    to the method" at a use site needs call syntax, which belongs to
 *    check.c, not this scope table. Any other same-kind collision (two
 *    methods, two fields, two top-level funcs, ...) is still an error. Not
 *    one of plan.md §7's ratified ambiguities — found while running the
 *    collector against the real corpus, treated as its own narrow rule.
 *
 * 5. Member-path (`a.b.c`) resolution only walks STATIC structure: local
 *    variable vs. type vs. package, per plan.md §4.3 ("tenta variável → tipo
 *    → prefixo de pacote"). Once the head resolves to a value (a local,
 *    param or field — anything needing an inferred/declared type to go
 *    further), resolution stops there; walking further segments needs type
 *    information that doesn't exist until check.c (P2.5) runs. This matches
 *    the corpus: qualified chains in real code are always TYPE.member
 *    (`Console.printline(...)`), never multi-package-segment value chains.
 */
#ifndef SNOVAC_RESOLVE_H
#define SNOVAC_RESOLVE_H

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"
#include "package.h"
#include "symbol.h"
#include "types.h"

/* New in this module — see package.h for why 108/109 and parse_internal.h
 * for why 100/101 are off-limits. The 110-119 and 121-128 sub-ranges are
 * already live elsewhere (const-time-literal-validation, pulsar-functions),
 * confirmed by grep before picking 120. */
#define SNOVA_DUPLICATE_DECL 120

/* Reused as-is — already fixture-backed with exactly this meaning. */
#define SNOVA_UNDECLARED_NAME 23  /* value position: not found anywhere */
#define SNOVA_UNKNOWN_TYPE 27     /* type position: not found anywhere */
#define SNOVA_TYPE_NOT_IMPORTED 121 /* type position: found, but not imported */
/* Warning, not an error: `Int` for `int` and the five other legacy
 * capitalized spellings. Same code and wording the Rust frontend uses
 * (crates/snovalang/src/native/selfcheck/mod.rs). */
#define SNOVA_LEGACY_TYPE_SPELLING 11

/* Declaration-shape rules. docs/snovalang-diagnostics.md assigns these to
 * "P1 parser (or P2 if left as a parse-then-validate check)"; they are done
 * here, during collection, precisely so the closed P1 gate's parser is not
 * reopened — the parser keeps recording `func`/`method` faithfully and this
 * pass judges the placement. `func` inside an `extension` body is legal
 * (tests/compile-pass/extension.snova) and is not reported. */
#define SNOVA_FUNC_IN_TYPE_BODY 30
#define SNOVA_METHOD_AT_TOP_LEVEL 31

typedef struct SnPackageScopeEntry SnPackageScopeEntry;
struct SnPackageScopeEntry {
    const char *package_name; /* interned */
    SnScope *scope;           /* top-level decls of this package */
    SnPackageScopeEntry *next;
};

typedef struct SnTypeScopeEntry SnTypeScopeEntry;
struct SnTypeScopeEntry {
    const SnDecl *type_decl; /* CLASS / STRUCT / INTERFACE / ENUM */
    SnScope *member_scope;   /* fields, methods, consts, variants — own only,
                               * NOT including inherited members; lookup
                               * walks supertypes explicitly */
    SnTypeScopeEntry *next;
};

typedef struct {
    SnArena *arena;
    SnInternTable *intern;
    SnDiagSink *diag;
    SnPackageGraph *graph;
    SnTypeTable *types;

    SnScope *prelude_scope; /* Option, Result — see scope note 2/3 above */
    SnPackageScopeEntry *packages;
    SnTypeScopeEntry *type_scopes;

    /* File sn_resolver_collect() is currently parsing; stamped onto every
     * symbol it defines so later phases can attribute a diagnostic to the
     * file that actually declared the symbol. NULL outside collection. */
    const SnDiagFile *current_origin;
} SnResolver;

void sn_resolver_init(SnResolver *r, SnArena *a, SnInternTable *it,
                       SnDiagSink *diag, SnPackageGraph *graph,
                       SnTypeTable *types);

/* Re-parses every file in the graph and collects top-level + member
 * declarations into per-package and per-type scopes. Two passes internally:
 * types/funcs/consts first (creating empty member scopes for types), then
 * `extension` blocks (merged into the matching same-package type's member
 * scope — this is how Option/Result actually get their methods; the enum
 * body itself only has variants). Duplicate top-level names within one
 * package, or duplicate member names within one type, emit
 * SNOVA_DUPLICATE_DECL. Returns the number of SnPackageFile entries skipped
 * for reason 1 above (multi-section) — counted per section, not per
 * physical file, so a file with 2 `package` lines contributes 2 to this
 * count, not 1 (package.c already split it into 2 SnPackageFile entries). */
size_t sn_resolver_collect(SnResolver *r);

/* Builds the implicit prelude (Option/Result only — see scope note 2/3).
 * Call after sn_resolver_collect(). */
void sn_resolver_build_prelude(SnResolver *r);

SnScope *sn_resolver_package_scope(const SnResolver *r, const char *package_name);
SnScope *sn_resolver_type_scope(const SnResolver *r, const SnDecl *type_decl);

/* Value-position identifier resolution, plan.md §4.3's 5 levels: local scope,
 * enclosing type's members (incl. single-inheritance walk — plan.md's
 * ratified ambiguity 5), current package, imports (source order, first
 * match wins — ambiguous imports are not detected), prelude. `local` and
 * `enclosing_type` may be NULL. `imports` holds interned package-name
 * `const char*` (e.g. SnUnit.imports). Emits SNOVA_UNDECLARED_NAME and
 * returns NULL when nothing matches. */
SnSymbol *sn_resolve_ident(SnResolver *r, const char *current_package,
                            const SnScope *local, const SnDecl *enclosing_type,
                            const SnList *imports, const char *name,
                            SnSpan span);

/* Type-position name resolution. Primitive names short-circuit to types.c's
 * singletons before any scope lookup (scope note 2). Otherwise tries the
 * same order as sn_resolve_ident (minus the local-scope level, which has no
 * meaning for a type name) and, on failure, searches the WHOLE graph before
 * giving up — a hit there means "real, but not imported"
 * (SNOVA_TYPE_NOT_IMPORTED) rather than SNOVA_UNKNOWN_TYPE. Returns NULL on
 * either failure (the diagnostic distinguishes them; the return value
 * doesn't need to). */
SnTypeRep *sn_resolve_type_name(SnResolver *r, const char *current_package,
                                 const SnList *imports, const char *name,
                                 SnSpan span);

/* `path` is a dotted qualified name (e.g. from parse_qualified), already
 * split into its interned segments by the caller. Resolves the head via
 * variable -> type -> package-prefix, per plan.md §4.3; see scope note 4 for
 * exactly where this stops. Returns the symbol the head resolved to (which
 * may represent only a prefix of `path` — the caller/check.c walks the
 * rest), or NULL with a diagnostic already emitted. */
SnSymbol *sn_resolve_member_path(SnResolver *r, const char *current_package,
                                  const SnScope *local,
                                  const SnDecl *enclosing_type,
                                  const SnList *imports, const SnList *segments,
                                  SnSpan span);

#endif /* SNOVAC_RESOLVE_H */
