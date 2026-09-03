#!/bin/sh
# run.sh — assertions for the lexer decisions that are easy to get wrong.
#
# Each case encodes a fact measured from the real corpus. If one of these
# regresses, the lexer has silently changed the language.

set -eu

SNOVAC="${1:-build/snovac}"
DIR="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0

assert() { # name, expected-count, actual-count
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'FAIL %s: expected %s, got %s\n' "$1" "$2" "$3"
  fi
}

toks() { "$SNOVAC" --emit=tokens "$DIR/lex/$1"; }

# Echoes a command's exit status without tripping `set -e`, which is inherited
# by command substitution and would otherwise abort the subshell at the
# failing command, before the status could be printed.
rc_of() {
  if "$@" >/dev/null 2>&1; then echo 0; else echo $?; fi
}

# `Task<Result<unit, DataError>>` must close as two separate `>` tokens.
# Snovalang has no shift operators; every `>>` in the corpus is a generic close.
assert "generics: no >> token" 2 "$(toks generics.snova | grep -c '^ *[0-9]*:[0-9]* *> *$')"

# `1.toString()` — the `.` after an int literal is member access, not a fraction.
assert "numbers: 1.toString splits" 1 \
  "$(toks numbers.snova | awk '$1=="6:9"' | grep -c 'int literal')"
assert "numbers: long suffix"    1 "$(toks numbers.snova | grep -c 'long literal')"
assert "numbers: 1.5 and 1e9"    2 "$(toks numbers.snova | grep -c 'double literal')"
assert "numbers: hex is int"     1 "$(toks numbers.snova | grep -c '0xFFFF')"

# `$$` is a literal dollar and must NOT mark the string interpolated;
# a quote inside `${...}` must not terminate the enclosing string.
assert "interp: exactly 2 interpolated" 2 \
  "$(toks interp.snova | grep -c '\[interpolated\]')"
assert "interp: 5 strings total" 5 \
  "$(toks interp.snova | grep -c 'string literal')"

# `and` is a method name in the corpus, so it must lex as an identifier.
assert "softkw: and is identifier" 1 \
  "$(toks softkw.snova | grep -c 'identifier *and$')"

# Property accessors: the absence of an accessor is the whole point of the
# feature, so assert that omitted ones stay omitted and are not defaulted in.
PROPS="$DIR/compile-pass/property_accessors.snova"
[ -f "$PROPS" ] || PROPS="$DIR/../../tests/compile-pass/property_accessors.snova"
if [ -f "$PROPS" ]; then
  props() { "$SNOVAC" --emit=ast "$PROPS"; }
  assert "props: parses clean" 0 "$(props >/dev/null 2>&1; echo $?)"
  assert "props: read-only field has get and no set" 1 \
    "$(props | grep -c 'private let id: string { get }')"
  assert "props: write-only field has set and no get" 1 \
    "$(props | grep -c 'var token: string { set }')"
  assert "props: empty block is not both accessors" 1 \
    "$(props | grep -c 'let salt: string { }')"
  assert "props: no block stays a plain field" 1 \
    "$(props | grep -c 'let attempts: int$')"
  assert "props: projections recorded on both sides" 1 \
    "$(props | grep -c 'var score: int { get: (\.\.\.) set: (\.\.\.) }')"
fi

# End-to-end execution against the repository's own run-pass fixtures. These
# assert real program output, not parser shape.
RP="$DIR/run-pass"
[ -d "$RP" ] || RP="$DIR/../../tests/run-pass"
for name in hello string_comment_url array_field_access counter extension_invocation; do
  if [ -f "$RP/$name.snova" ] && [ -f "$RP/$name.stdout" ]; then
    got="$("$SNOVAC" run "$RP/$name.snova" 2>&1 || true)"
    assert "run: $name" "$(cat "$RP/$name.stdout")" "$got"
  fi
done

if [ -z "${SNOVA_BUILTIN_DIR:-}" ]; then
  if [ -d "$DIR/../builtin" ]; then
    export SNOVA_BUILTIN_DIR="$DIR/../builtin"
  elif [ -d "$DIR/../../builtin" ]; then
    export SNOVA_BUILTIN_DIR="$DIR/../../builtin"
  fi
fi

# Project-wide analysis: the regression that motivated it was a syntax error
# in a NON-entry file passing every gate and the program running anyway,
# because only the entry file was ever looked at. These assert that the whole
# project is analysed, and that a clean one still passes.
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM
mkdir -p "$PROJ/src/app"
cat > "$PROJ/src/app/Main.snova" <<'EOF'
package app

import app.Models

func main(): int {
    return 0
}
EOF
cat > "$PROJ/src/app/Models.snova" <<'EOF'
package app

public struct Config {
    public let name: string
}
EOF

assert "project: clean project passes" 0 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova")"

# The entry file itself stays valid; only the sibling module is broken.
cat > "$PROJ/src/app/Models.snova" <<'EOF'
package app

public struct Config
    public let name: string
}
EOF

assert "project: sibling syntax error is caught" 1 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova")"
assert "project: single-file mode still cannot see it" 0 \
  "$(rc_of "$SNOVAC" --check-parse "$PROJ/src/app/Main.snova")"
assert "project: error names the sibling, not the entry file" 1 \
  "$("$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova" 2>&1 | grep -c 'Models.snova:5')"

# `import a.b.C` names a SYMBOL in package `a.b` as often as it names a whole
# package; treating the symbol spelling as a missing package made SNOVA0050
# fire on correct code across an entire project.
mkdir -p "$PROJ/src/lib"
cat > "$PROJ/src/lib/Util.snova" <<'EOF'
package app.lib

public struct Helper {
    public let id: int
}
EOF
cat > "$PROJ/src/app/Models.snova" <<'EOF'
package app

import app.lib.Helper

public struct Config {
    public let name: string
}
EOF

assert "project: symbol import resolves to its package" 0 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova")"

# The above only proves the IMPORT LINE doesn't trip SNOVA0050 (package-graph
# linking); it never actually referenced `Helper` by name anywhere, so it
# could not catch resolve.c's own copy of the same bug. Every import-scope
# lookup in resolve.c (sn_resolve_ident, sn_resolve_type_name,
# sn_resolve_member_path) looked up `sn_resolver_package_scope(r, imp_pkg)`
# with the RAW import string as written — for `import app.lib.Helper` that's
# "app.lib.Helper", which matches no real package (the real one is
# "app.lib"), so the exact-pointer-equality scope lookup always failed
# silently. Only the bare `import app.lib` form ever worked.
#
# This needs its own project (with a real snova.toml, not $PROJ, which has
# none): without a manifest, source_root falls back to just the entry file's
# own directory (project_discover's documented behavior), so a sibling
# package directory like src/lib/ is never scanned at all — resolving
# `app.lib.Helper` would then hit resolve_import_target()'s longest-match
# fallthrough all the way down to "app" itself (the importer's own,
# genuinely-scanned package), which happens to exist and masks the bug
# behind a coincidental self-package match instead of a real cross-package
# one. A manifest makes source_root the project's whole `src/`, covering
# both directories for real.
SYMPROJ="$(mktemp -d)"
mkdir -p "$SYMPROJ/src/app" "$SYMPROJ/src/lib"
cat > "$SYMPROJ/snova.toml" <<'EOF'
[package]
name = "symproj"
EOF
cat > "$SYMPROJ/src/lib/Util.snova" <<'EOF'
package app.lib

public struct Helper {
    public let id: int
}
EOF
cat > "$SYMPROJ/src/app/Main.snova" <<'EOF'
package app

import app.lib.Helper

func main(): int {
    let h = Helper { id: 1 }
    return h.id
}
EOF

assert "project: symbol import resolves the symbol itself, not just the package" 0 \
  "$(rc_of "$SNOVAC" check --project "$SYMPROJ/src/app/Main.snova")"
rm -rf "$SYMPROJ"

cat > "$PROJ/src/app/Main.snova" <<'EOF'
package app

import app.Models

func main(): int {
    return 0
}
EOF

# No prefix of this names a declared package, and it is not under a
# toolchain-provided namespace, so it stays a reported error.
cat > "$PROJ/src/app/Models.snova" <<'EOF'
package app

import nowhere.at.All

public struct Config {
    public let name: string
}
EOF

assert "project: genuinely missing import still reported" 1 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova")"

# `builtin.*` / `stdlib.*` packages actually registered in
# `compiler/src/lsp/NativePackages.snova` (and generated into
# `builtin/native-packages.list` by `scripts/gen-packages.sh`) have no
# `.snova` file anywhere snovac can see, so absence there must not be
# reported as a missing package.
cat > "$PROJ/src/app/Models.snova" <<'EOF'
package app

import builtin.syntax.Syntax

public struct Config {
    public let name: string
}
EOF

assert "project: registered native package is not judged" 0 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova")"

# `builtin.http.Http` is NOT in native-packages.list (no such package is
# registered anywhere yet) and has no `.snova` file either — this used to be
# silently accepted by a blanket `builtin.*`/`stdlib.*` prefix rule (the exact
# gap `tests/conformance/` flagged as "missing_import não é rejeitado por
# snovac check"). Fixed by sn_pkggraph_load_native_manifest(): only names
# actually present in the manifest are treated as toolchain-provided now.
cat > "$PROJ/src/app/Models.snova" <<'EOF'
package app

import builtin.http.Http

public struct Config {
    public let name: string
}
EOF

assert "project: unregistered builtin.* namespace is still reported" 1 \
  "$(rc_of "$SNOVAC" check --project --no-typecheck "$PROJ/src/app/Main.snova")"

# Native backend, target detection, and sandbox testing
assert "target: --target-info outputs host and target" 1 \
  "$("$SNOVAC" --target-info | grep -c 'Target OS:' || true)"

assert "target: env override changes target OS" 1 \
  "$(SNOVA_TARGET_OS=freebsd "$SNOVAC" --target-info | grep -c 'freebsd (overridden)' || true)"

# Compile standalone native binary and test execution
BUILD_OUT="$PROJ/bin_hello"
assert "build: compile standalone native binary" 0 \
  "$(rc_of "$SNOVAC" build "$RP/hello.snova" -o "$BUILD_OUT")"

if [ -f "$BUILD_OUT" ]; then
  got_native="$("$BUILD_OUT" 2>&1 || true)"
  assert "build: native binary output matches run-pass" "$(cat "$RP/hello.stdout")" "$got_native"
  rm -f "$BUILD_OUT"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
