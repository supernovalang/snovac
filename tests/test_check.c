/* test_check.c — expression/statement type checking.
 *
 * specs/20260719/snovac-p2-resolver-typechecker/plan.md §8 steps 7-8
 * verification, scoped to what check.c actually covers (see check.h's file
 * header for the explicit list of what's solid vs. documented-gap).
 * Standalone C binary, same rationale as the other test_*.c files.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../arena.h"
#include "../check.h"
#include "../diag.h"
#include "../intern.h"
#include "../package.h"
#include "../resolve.h"
#include "../symbol.h"
#include "../types.h"

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

typedef struct {
    SnArena arena;
    SnInternTable intern;
    char *diag_buf;
    size_t diag_buf_len;
    SnDiagSink diag;
    SnPackageGraph graph;
    SnTypeTable types;
    SnResolver resolver;
    SnChecker checker;
} World;

static void world_init(World *w) {
    sn_arena_init(&w->arena, 0);
    sn_intern_init(&w->intern, &w->arena);
    sn_diag_init(&w->diag, "<test>", "", 0);
    w->diag.out = open_memstream(&w->diag_buf, &w->diag_buf_len);
    sn_pkggraph_init(&w->graph, &w->arena, &w->intern, &w->diag);
    sn_types_init(&w->types, &w->arena);
    sn_resolver_init(&w->resolver, &w->arena, &w->intern, &w->diag, &w->graph, &w->types);
    sn_checker_init(&w->checker, &w->arena, &w->intern, &w->diag, &w->resolver, &w->types);
}

static void world_finish_diag(World *w) { fclose(w->diag.out); }

static int diag_has_code(World *w, const char *code /* e.g. "SNOVA0131" */) {
    return w->diag_buf && strstr(w->diag_buf, code) != NULL;
}

/* Scans `dir`, collects, builds the prelude, then checks the body of
 * top-level func `func_name` in `package_name`. Leaves diag output flushed
 * into w->diag_buf so callers can inspect error_count and/or diag_has_code. */
static void run_check_on_func(World *w, const char *dir, const char *package_name,
                              const char *func_name) {
    sn_pkggraph_scan_root(&w->graph, dir);
    sn_resolver_collect(&w->resolver);
    sn_resolver_build_prelude(&w->resolver);

    const char *pkg = sn_intern_cstr(&w->intern, package_name);
    SnScope *pkg_scope = sn_resolver_package_scope(&w->resolver, pkg);
    SnSymbol *fn_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w->intern, func_name))
                  : NULL;
    if (!fn_sym) {
        printf("FAIL setup: %s.%s not collected\n", package_name, func_name);
        return;
    }

    w->checker.current_package = pkg;
    w->checker.current_imports = NULL;
    w->checker.enclosing_type = NULL;
    sn_check_decl_body(&w->checker, fn_sym->decl);
    world_finish_diag(w);
}

/* ── scenarios ────────────────────────────────────────────────────────────── */

static void test_literals_and_arithmetic(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lit", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.lit\n\n"
              "func good(): int {\n"
              "    let a = 1\n"
              "    let b = 2\n"
              "    let c = a + b\n"
              "    return c\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lit", "good");
    CHECK("literals: int + int, matching return type — no errors", w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_no_implicit_promotion(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/promo", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.promo\n\n"
              "func bad(): int {\n"
              "    let a: int = 1\n"
              "    let b: long = 2L\n"
              "    let c = a + b\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.promo", "bad");
    CHECK("no promotion: int + long is exactly one error", w.diag.error_count == 1);
    CHECK("no promotion: emits SNOVA0133 (binary type mismatch)",
          diag_has_code(&w, "SNOVA0133"));

    sn_arena_free(&w.arena);
}

static void test_string_concat(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/strcat", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.strcat\n\n"
              "func greet(): string {\n"
              "    let name = \"world\"\n"
              "    return \"hello \" + name\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.strcat", "greet");
    CHECK("string concat: string + string returns string, matches return type — no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_unary_operators(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/unary", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.unary\n\n"
              "func good(): bool {\n"
              "    let x = 5\n"
              "    let y = -x\n"
              "    let flag = true\n"
              "    return !flag\n"
              "}\n\n"
              "func bad(): int {\n"
              "    let flag = true\n"
              "    let broken = -flag\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.unary", "good");
    CHECK("unary: -int and !bool are both fine", w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.unary", "bad");
    CHECK("unary: -bool is exactly one error", w2.diag.error_count == 1);
    CHECK("unary: emits SNOVA0134", diag_has_code(&w2, "SNOVA0134"));
    sn_arena_free(&w2.arena);
}

static void test_assignment_mutability(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/mut", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.mut\n\n"
              "func reassignVar(): int {\n"
              "    var total = 0\n"
              "    total = 5\n"
              "    return total\n"
              "}\n\n"
              "func reassignLet(): int {\n"
              "    let total = 0\n"
              "    total = 5\n"
              "    return total\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.mut", "reassignVar");
    CHECK("mutability: reassigning a `var` is fine", w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.mut", "reassignLet");
    CHECK("mutability: reassigning a `let` is exactly one error", w2.diag.error_count == 1);
    CHECK("mutability: emits SNOVA0047 (already fixture-backed code, reused)",
          diag_has_code(&w2, "SNOVA0047"));
    sn_arena_free(&w2.arena);
}

static void test_member_access_with_inheritance(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/member", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.member\n\n"
              "class Animal {\n"
              "    method speak(): string {\n"
              "        return \"...\"\n"
              "    }\n"
              "}\n\n"
              "class Dog extends Animal {\n"
              "    method bark(): string {\n"
              "        return \"woof\"\n"
              "    }\n"
              "}\n\n"
              "func makeNoise(d: Dog): string {\n"
              "    return d.speak()\n"
              "}\n\n"
              "func badMember(d: Dog): string {\n"
              "    return d.nonexistent()\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.member", "makeNoise");
    CHECK("member access: inherited method call resolves — no errors",
          w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.member", "badMember");
    CHECK("member access: unknown member is exactly one error", w2.diag.error_count == 1);
    CHECK("member access: emits SNOVA0028 (already fixture-backed code, reused)",
          diag_has_code(&w2, "SNOVA0028"));
    sn_arena_free(&w2.arena);
}

static void test_call_arity_and_arg_types(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/call", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.call\n\n"
              "func add(a: int, b: int): int {\n"
              "    return a + b\n"
              "}\n\n"
              "func good(): int {\n"
              "    return add(1, 2)\n"
              "}\n\n"
              "func wrongArity(): int {\n"
              "    return add(1)\n"
              "}\n\n"
              "func wrongArgType(): int {\n"
              "    return add(1, \"two\")\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.call", "good");
    CHECK("call: correct arity and types — no errors", w.diag.error_count == 0);
    sn_arena_free(&w.arena);

    World w2;
    world_init(&w2);
    run_check_on_func(&w2, dir, "pkg.call", "wrongArity");
    CHECK("call: arity mismatch is exactly one error", w2.diag.error_count == 1);
    CHECK("call: emits SNOVA0131", diag_has_code(&w2, "SNOVA0131"));
    sn_arena_free(&w2.arena);

    World w3;
    world_init(&w3);
    run_check_on_func(&w3, dir, "pkg.call", "wrongArgType");
    CHECK("call: arg type mismatch is exactly one error", w3.diag.error_count == 1);
    CHECK("call: emits SNOVA0132", diag_has_code(&w3, "SNOVA0132"));
    sn_arena_free(&w3.arena);
}

static void test_variant_construction(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/variant", tmp);
    mkdir(dir, 0755);
    write_file(dir, "types.snova",
              "package builtin.types.Types\n\n"
              "enum Option {\n"
              "    Some(value: int),\n"
              "    None,\n"
              "}\n");
    write_file(dir, "user.snova",
              "package pkg.user\n\n"
              "func makeSome(): int {\n"
              "    let x = Some(1)\n"
              "    return 1\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.user", "makeSome");
    CHECK("variant construction: `Some(1)` resolves through the prelude with no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_condition_must_be_bool(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/cond", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.cond\n\n"
              "func bad(): int {\n"
              "    let n = 5\n"
              "    if n {\n"
              "        return 1\n"
              "    }\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.cond", "bad");
    CHECK("condition: non-bool `if` condition is exactly one error", w.diag.error_count == 1);
    CHECK("condition: emits SNOVA0135", diag_has_code(&w, "SNOVA0135"));

    sn_arena_free(&w.arena);
}

static void test_return_type_mismatch(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ret", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.ret\n\n"
              "func bad(): int {\n"
              "    return \"not an int\"\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.ret", "bad");
    CHECK("return: mismatched return type is exactly one error", w.diag.error_count == 1);
    CHECK("return: emits SNOVA0136", diag_has_code(&w, "SNOVA0136"));

    sn_arena_free(&w.arena);
}

static void test_let_type_mismatch(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lettype", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.lettype\n\n"
              "func bad(): int {\n"
              "    let x: int = \"nope\"\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lettype", "bad");
    CHECK("let type: declared type disagreeing with initializer is exactly one error",
          w.diag.error_count == 1);
    CHECK("let type: emits SNOVA0138", diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_empty_array_literal(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arr", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.arr\n\n"
              "func bad(): int {\n"
              "    var xs = []\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arr", "bad");
    CHECK("array: empty untyped literal is exactly one error", w.diag.error_count == 1);
    CHECK("array: emits SNOVA0100 (reused, matches the real fixture's meaning exactly)",
          diag_has_code(&w, "SNOVA0100"));

    sn_arena_free(&w.arena);
}

/* ── intrinsics, constructors and literal typing (2026-07-25) ─────────────── */

static void test_array_intrinsic(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrint", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.arrint\n\n"
              "func good(xs: Array<int>): int {\n"
              "    let n = xs.len()\n"
              "    let first = xs.get(0)\n"
              "    let joined = xs.join(\", \")\n"
              "    let alt: int[] = xs\n"
              "    return n + first\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arrint", "good");
    CHECK("array intrinsic: Array<int> resolves and carries len/get/join — no errors",
          w.diag.error_count == 0);
    CHECK("array intrinsic: `int[]` is the same hash-consed type as `Array<int>`",
          !diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_array_intrinsic_unknown_member(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrbad", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.arrbad\n\n"
              "func bad(xs: Array<int>): int {\n"
              "    return xs.notAMember()\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arrbad", "bad");
    CHECK("array intrinsic: a member outside the table is still SNOVA0028",
          diag_has_code(&w, "SNOVA0028"));

    sn_arena_free(&w.arena);
}

static void test_array_literal_with_declared_type(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/arrann", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.arrann\n\n"
              "func good(): int {\n"
              "    var ids: Array<string> = []\n"
              "    var xs: string[] = []\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.arrann", "good");
    CHECK("array: an annotated binding supplies the element type SNOVA0100 asks for",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_scalar_and_string_members(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/prim", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.prim\n\n"
              "func good(): int {\n"
              "    let n = 7\n"
              "    let s = n.toString()\n"
              "    let up = s.trim()\n"
              "    let len = up.length()\n"
              "    let empty = s.isEmpty()\n"
              "    return len\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.prim", "good");
    CHECK("primitives: toString/trim/length/isEmpty resolve with their modeled results",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_constructor_call(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ctor", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.ctor\n\n"
              "struct Point {\n"
              "    public let x: decimal\n"
              "    public let y: decimal\n"
              "}\n\n"
              "func good(): int {\n"
              "    let p = Point(3.0, 4.0)\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.ctor", "good");
    CHECK("constructor: the field list is the signature, and `3.0` adopts `decimal`",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_constructor_arity(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/ctorbad", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.ctorbad\n\n"
              "struct Point {\n"
              "    public let x: int\n"
              "    public let y: int\n"
              "}\n\n"
              "func bad(): int {\n"
              "    let p = Point(1)\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.ctorbad", "bad");
    CHECK("constructor: too few arguments is exactly one error", w.diag.error_count == 1);
    CHECK("constructor: emits SNOVA0131 (arity)", diag_has_code(&w, "SNOVA0131"));

    sn_arena_free(&w.arena);
}

static void test_lambda_value_call(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lamcall", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.lamcall\n\n"
              "func good(n: int): int {\n"
              "    let double = (x: int) -> x * 2\n"
              "    return double(n)\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lamcall", "good");
    CHECK("call: a function-typed local is callable, not SNOVA0137",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_lambda_value_call_arity(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/lamarity", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.lamarity\n\n"
              "func bad(n: int): int {\n"
              "    let double = (x: int) -> x * 2\n"
              "    return double(n, n)\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.lamarity", "bad");
    CHECK("call: a function-typed local still has its arity checked",
          diag_has_code(&w, "SNOVA0131"));

    sn_arena_free(&w.arena);
}

static void test_subtype_assignability(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/sub", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.sub\n\n"
              "class Animal {\n"
              "    method speak(): string = \"...\"\n"
              "}\n\n"
              "class Dog : Animal {\n"
              "    override method speak(): string = \"woof\"\n"
              "}\n\n"
              "func good(): int {\n"
              "    let pet: Animal = Dog()\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.sub", "good");
    CHECK("subtyping: a Dog initializes an Animal binding", w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_subtype_not_symmetric(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/subrev", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.subrev\n\n"
              "class Animal {\n"
              "    method speak(): string = \"...\"\n"
              "}\n\n"
              "class Dog : Animal {\n"
              "    override method speak(): string = \"woof\"\n"
              "}\n\n"
              "func bad(a: Animal): int {\n"
              "    let pet: Dog = a\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.subrev", "bad");
    CHECK("subtyping: assignability is directional — an Animal is not a Dog",
          diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_literal_suffix_still_strict(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/suffix", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.suffix\n\n"
              "func bad(): int {\n"
              "    let a: int = 2L\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.suffix", "bad");
    CHECK("literals: a SUFFIXED literal is committed and still mismatches",
          diag_has_code(&w, "SNOVA0138"));

    sn_arena_free(&w.arena);
}

static void test_func_in_class_body(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/shape", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.shape\n\n"
              "class Greeter {\n"
              "    func greet(): string {\n"
              "        return \"hello\"\n"
              "    }\n"
              "}\n\n"
              "method badTopLevel(): int {\n"
              "    return 0\n"
              "}\n\n"
              "func main(): int {\n"
              "    return 0\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    world_finish_diag(&w);
    CHECK("shape: `func` inside a class body emits SNOVA0030",
          diag_has_code(&w, "SNOVA0030"));
    CHECK("shape: `method` at the top level emits SNOVA0031",
          diag_has_code(&w, "SNOVA0031"));

    sn_arena_free(&w.arena);
}

static void test_extension_func_is_legal(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/extfn", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.extfn\n\n"
              "class Counter {\n"
              "    private var value: int\n"
              "}\n\n"
              "extension Counter {\n"
              "    func isZero(): bool = value == 0\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    world_finish_diag(&w);
    CHECK("shape: `func` inside an `extension` body is legal — no SNOVA0030",
          !diag_has_code(&w, "SNOVA0030"));

    sn_arena_free(&w.arena);
}

static void test_cast_and_is(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/castis", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.castis\n\n"
              "func good(): long {\n"
              "    let x = 5\n"
              "    let y = x as long\n"
              "    let flag = x is int\n"
              "    return y\n"
              "}\n");

    World w;
    world_init(&w);
    run_check_on_func(&w, dir, "pkg.castis", "good");
    CHECK("cast/is: `as` trusts the annotation, `is` returns bool — no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

static void test_this_in_method(const char *tmp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/thistest", tmp);
    mkdir(dir, 0755);
    write_file(dir, "f.snova",
              "package pkg.thistest\n\n"
              "class Counter {\n"
              "    method self(): Counter {\n"
              "        return this\n"
              "    }\n"
              "}\n");

    World w;
    world_init(&w);
    sn_pkggraph_scan_root(&w.graph, dir);
    sn_resolver_collect(&w.resolver);
    sn_resolver_build_prelude(&w.resolver);

    const char *pkg = sn_intern_cstr(&w.intern, "pkg.thistest");
    SnScope *pkg_scope = sn_resolver_package_scope(&w.resolver, pkg);
    SnSymbol *counter_sym =
        pkg_scope ? sn_scope_lookup_local(pkg_scope, sn_intern_cstr(&w.intern, "Counter"))
                  : NULL;
    CHECK("this: Counter collected", counter_sym != NULL);
    SnScope *member_scope =
        counter_sym ? sn_resolver_type_scope(&w.resolver, counter_sym->decl) : NULL;
    SnSymbol *method_sym =
        member_scope ? sn_scope_lookup_local(member_scope, sn_intern_cstr(&w.intern, "self"))
                     : NULL;
    CHECK("this: self() method collected", method_sym != NULL);

    if (method_sym) {
        w.checker.current_package = pkg;
        w.checker.current_imports = NULL;
        w.checker.enclosing_type = counter_sym->decl;
        sn_check_decl_body(&w.checker, (SnDecl *)method_sym->decl);
    }
    world_finish_diag(&w);
    CHECK("this: `return this` inside a method matching its own return type — no errors",
          w.diag.error_count == 0);

    sn_arena_free(&w.arena);
}

int main(void) {
    char tmp[] = "/tmp/snovac_check_test_XXXXXX";
    if (!mkdtemp(tmp)) {
        printf("FAIL setup: mkdtemp failed\n");
        return 1;
    }

    test_literals_and_arithmetic(tmp);
    test_no_implicit_promotion(tmp);
    test_string_concat(tmp);
    test_unary_operators(tmp);
    test_assignment_mutability(tmp);
    test_member_access_with_inheritance(tmp);
    test_call_arity_and_arg_types(tmp);
    test_variant_construction(tmp);
    test_condition_must_be_bool(tmp);
    test_return_type_mismatch(tmp);
    test_let_type_mismatch(tmp);
    test_empty_array_literal(tmp);
    test_cast_and_is(tmp);
    test_this_in_method(tmp);
    test_array_intrinsic(tmp);
    test_array_intrinsic_unknown_member(tmp);
    test_array_literal_with_declared_type(tmp);
    test_scalar_and_string_members(tmp);
    test_constructor_call(tmp);
    test_constructor_arity(tmp);
    test_lambda_value_call(tmp);
    test_lambda_value_call_arity(tmp);
    test_subtype_assignability(tmp);
    test_subtype_not_symmetric(tmp);
    test_literal_suffix_still_strict(tmp);
    test_func_in_class_body(tmp);
    test_extension_func_is_legal(tmp);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
