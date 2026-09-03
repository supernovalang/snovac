#!/bin/sh
# snovac-conformance.sh — runs snovac over the whole .snova corpus.
#
# The oracle is the corpus itself, never the Rust Stage 0: that component
# matches source text with substring scans and is not a reliable reference for
# language behaviour (see specs/20260719/snovac-c-toolchain/plan.md §0).
#
# Two gates, in order:
#   1. lexical  (--check-lex)   over the full corpus, including compile-fail;
#   2. syntactic (--check-parse) over the same corpus MINUS tests/compile-fail*,
#      because those fixtures are required to fail — most only at P2 (semantic
#      errors), so they are not a parser metric either way.
#
# Exit status is 0 only at 100% on both gates, so this doubles as the P1 gate.

set -eu

SNOVAC="${1:-build/snovac}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$ROOT"

case "$SNOVAC" in
  /*) ;;
  *)  SNOVAC="$(cd "$(dirname "$SNOVAC")" && pwd)/$(basename "$SNOVAC")" ;;
esac

if [ ! -x "$SNOVAC" ]; then
  echo "error: snovac not built at $SNOVAC (run: make -C snovac)" >&2
  exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

all="$work/all"
parseable="$work/parseable"
: > "$all"

# Corpus scope: the compiler is independent of the Sonar libs and of the
# stdlib packages, so neither is scanned here. `examples/` is demonstration
# code, not normative corpus. What remains is what the compiler owns —
# including `builtin/`, the vendored modules the compiler itself depends on
# (specs/20260719/builtin-module-vendoring, pendência 2).
for dir in "$ROOT/tests" "$ROOT/compiler/src" "$ROOT/builtin"; do
  [ -d "$dir" ] || continue
  find "$dir" -name '*.snova' -type f >> "$all"
done
sort -o "$all" "$all"

grep -v '/compile-fail' "$all" | grep -v '/tests/conformance' > "$parseable" || : > "$parseable"

run_gate() {
  # $1 = gate label, $2 = snovac flag, $3 = file list
  gate_failed="$work/failed-$1"
  : > "$gate_failed"
  gate_total=0
  while IFS= read -r f; do
    gate_total=$((gate_total + 1))
    if ! "$SNOVAC" "$2" "$f" >/dev/null 2>&1; then
      printf '%s\n' "$f" >> "$gate_failed"
    fi
  done < "$3"

  gate_bad=$(wc -l < "$gate_failed" | tr -d ' ')
  gate_ok=$((gate_total - gate_bad))

  echo "snovac $1 conformance"
  echo "  corpus : $gate_total files"
  echo "  passed : $gate_ok"
  echo "  failed : $gate_bad"

  if [ "$gate_total" -eq 0 ]; then
    echo "error: corpus is empty — nothing was checked" >&2
    exit 2
  fi

  if [ "$gate_bad" -gt 0 ]; then
    echo
    echo "failing files:"
    sed "s|^$REPO/||" "$gate_failed" | head -40 | sed 's|^|  |'
    [ "$gate_bad" -gt 40 ] && echo "  ... and $((gate_bad - 40)) more"
    exit 1
  fi
  echo
}

run_gate lexical --check-lex "$all"
run_gate syntactic --check-parse "$parseable"

echo "P1 lexical gate:   PASS (100%)"
echo "P1 syntactic gate: PASS (100%)"
exit 0
