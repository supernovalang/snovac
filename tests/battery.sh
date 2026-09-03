#!/bin/sh
# battery.sh — P2.6 gate: run `snovac check` over every tests/compile-pass and
# tests/compile-fail fixture and report what actually happens.
#
# `check` scans the fixture plus the whole builtin/ tree, so most runs also
# surface diagnostics from builtin files. Those are NOT the fixture's verdict:
# the numbers below count only diagnostics whose `--> path` is the fixture
# itself. Cross-file attribution is what makes that split possible at all
# (SnDiagFile / SnSymbol.origin) — before it, every diagnostic claimed to come
# from the entry file.
#
# usage: sh tests/battery.sh [path/to/snovac] [path/to/tests]
set -eu

SNOVAC="${1:-build/snovac}"
ROOT="${2:-tests}"
[ -d "$ROOT" ] || ROOT="$PWD/tests"
case "$SNOVAC" in /*) ;; *) SNOVAC="$PWD/$SNOVAC" ;; esac
case "$ROOT" in /*) ;; *) ROOT="$PWD/$ROOT" ;; esac

NO_COLOR=1
export NO_COLOR

# One `check` run per fixture, cached — the run scans all of builtin/ and is
# by far the expensive part.
CACHE=$(mktemp -d)
trap 'rm -rf "$CACHE"' EXIT INT TERM

check_once() { # file -> path to its captured stderr
  out="$CACHE/$(printf '%s' "$1" | tr / _)"
  [ -f "$out" ] || "$SNOVAC" check "$1" 2>"$out" >/dev/null || true
  printf '%s' "$out"
}

# Errors reported against $1 itself, ignoring everything from builtin/ etc.
#
# Severity matters: a compile-pass fixture is allowed to emit WARNINGS (several
# use the capitalized `Int`/`Float` spellings, which resolve to the primitive
# with SNOVA011 advice — exactly what the shipped `snova check` does). Only
# `error[...]` lines are a verdict, so the severity of the diagnostic heading
# is carried down to its `-->` location line before counting.
own_errors() { # file -> count
  awk -v f="$1" '
        /^error\[/   { sev = "error"; next }
        /^warning\[/ { sev = "warning"; next }
        $0 ~ " --> " f ":" && sev == "error" { n++; sev = "" }
        END { print n + 0 }' "$(check_once "$1")"
}

# The SNOVA code the fixture's .stderr documents, normalized to an integer
# (the recorded text uses the Rust frontend's SNOVA050 spelling, snovac prints
# SNOVA0050). Empty when the fixture has no .stderr.
expected_code() { # file -> code or ""
  expected="${1%.snova}.stderr"
  [ -f "$expected" ] || return 0
  sed -n 's/.*SNOVA0*\([0-9][0-9]*\).*/\1/p' "$expected" | head -1
}

own_codes() { # file -> newline-separated ERROR codes reported against it
  awk -v f="$1" '
        /^error\[SNOVA/   { split($0, m, /SNOVA0*/); split(m[2], n, /\]/); code = n[1]; next }
        /^warning\[SNOVA/ { code = ""; next }
        $0 ~ " --> " f ":" && code != "" { print code + 0; code = "" }' "$(check_once "$1")"
}

pass_total=0; pass_ok=0
# Fixtures under compile-pass/packages/ import DOMAIN SLUGS
# (`builtin.http.Http`, `builtin.sql.Sql`, `builtin.async`, ...). Those are
# deliberately absent from the monorepo — crates/snovalang/build.rs only
# embeds BOOTSTRAP_PACKAGES from builtin/, and everything else "must come from
# `snova deps` / registry cache, never from the monorepo embed table". `snovac
# check` has no dependency-materialization step, so it cannot see them. They
# are counted apart rather than mixed into a single misleading percentage.
#
# The same gap also reaches one fixture outside that directory
# (missing_import_type_usage_ok.snova imports `builtin.http.Http`), so the test
# is "does this fixture report SNOVA050, package-not-found, against itself" —
# that diagnostic IS the dependency-materialization gap, by definition. It is
# only consulted for compile-pass fixtures; compile-fail keeps its own tally.
needs_materialized_deps() { # file
  case "$1" in */compile-pass/packages/*) return 0;; esac
  own_codes "$1" | grep -qx 50
}

deps_total=0; deps_clean=0
printf '\n== compile-pass (expect: no error attributed to the fixture) ==\n'
for f in $(find "$ROOT/compile-pass" -name '*.snova' | sort); do
  n=$(own_errors "$f")
  if needs_materialized_deps "$f"; then
    deps_total=$((deps_total + 1))
    [ "$n" -eq 0 ] && deps_clean=$((deps_clean + 1))
    continue
  fi
  pass_total=$((pass_total + 1))
  if [ "$n" -eq 0 ]; then
    pass_ok=$((pass_ok + 1))
  else
    printf '  FAIL %-58s %s own error(s)\n' "${f#$ROOT/}" "$n"
  fi
done

fail_total=0; fail_ok=0; fail_code_ok=0
printf '\n== compile-fail (expect: at least one error on the fixture) ==\n'
for f in $(find "$ROOT/compile-fail" -name '*.snova' | sort); do
  fail_total=$((fail_total + 1))
  n=$(own_errors "$f")
  if [ "$n" -eq 0 ]; then
    printf '  MISSED %-56s no error reported\n' "${f#$ROOT/}"
    continue
  fi
  fail_ok=$((fail_ok + 1))
  want=$(expected_code "$f")
  if [ -z "$want" ]; then
    continue
  fi
  if own_codes "$f" | grep -qx "$want"; then
    fail_code_ok=$((fail_code_ok + 1))
  else
    printf '  WRONG-CODE %-51s want SNOVA%04d, got %s\n' "${f#$ROOT/}" "$want" \
      "$(own_codes "$f" | sort -u | tr '\n' ',' | sed 's/,$//')"
  fi
done

printf '\n== P2.6 battery ==\n'
printf '  compile-pass clean          : %d/%d\n' "$pass_ok" "$pass_total"
printf '  compile-fail caught         : %d/%d\n' "$fail_ok" "$fail_total"
printf '  ...with the documented code : %d\n' "$fail_code_ok"
printf '  not checkable (needs `snova deps`, see note above): %d/%d clean\n' \
  "$deps_clean" "$deps_total"
printf '\n'
