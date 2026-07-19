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

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
