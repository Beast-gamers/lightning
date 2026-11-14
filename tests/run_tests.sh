#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LTNG_SRC="$ROOT/compiler/ltng"
LTNG="/tmp/opencode/ltng"
TMP="/tmp/opencode/ltng_tests"
mkdir -p "$TMP" /tmp/opencode

# Build compiler
(cd "$ROOT/compiler" && make >/dev/null)
cp "$LTNG_SRC" "$LTNG"
chmod +x "$LTNG" 2>/dev/null || true

PASS=0; FAIL=0; SKIP=0
fail() { echo "FAIL: $1"; echo "  $2"; FAIL=$((FAIL+1)); }
pass() { PASS=$((PASS+1)); }

run_case() {
  local name="$1" src="$2" expect="${3:-}" mode="${4:-run}"
  local base="$TMP/$(echo "$name" | tr '/ ' '__')"
  local exitfile="${src%.ltng}.exitcode"
  local stderrfile="${src%.ltng}.stderr"
  rm -f "$base" "$base.c" "$base.out" "$base.err" "$base.run.out" "$base.run.err"
  if [ "$mode" = "check" ]; then
    if "$LTNG" --check "$src" >"$base.out" 2>"$base.err"; then
      pass
    else
      fail "$name" "expected check pass; rc=$?; $(tail -n +1 "$base.err" | head -5 | tr '\n' ' ')"
    fi
    return
  fi
  if [ "$mode" = "check_fail" ]; then
    if "$LTNG" --check "$src" >"$base.out" 2>"$base.err"; then
      fail "$name" "expected check failure but succeeded"
    else
      pass
    fi
    return
  fi
  if [ "$mode" = "emit_c" ]; then
    if "$LTNG" --emit-c "$src" "$base.c" >"$base.out" 2>"$base.err" && [ -s "$base.c" ]; then
      pass
    else
      fail "$name" "emit-c failed; rc=$?; $(tail -n +1 "$base.err" | head -5 | tr '\n' ' ')"
    fi
    return
  fi
  # run (compile+execute native)
  if ! "$LTNG" "$src" "$base" >"$base.out" 2>"$base.err"; then
    fail "$name" "compile failed; rc=$?; $(tail -n +1 "$base.err" | head -8 | tr '\n' ' ')"
    return
  fi
  if [ ! -x "$base" ] && [ ! -f "$base" ]; then
    fail "$name" "no output binary produced"
    return
  fi
  chmod +x "$base" 2>/dev/null || true
  set +e
  "$base" >"$base.run.out" 2>"$base.run.err"
  rc=$?
  set -e
  local expected_rc=""
  if [ -f "$exitfile" ]; then
    expected_rc="$(tr -dc '0-9-' <"$exitfile")"
  fi
  if [ -n "$expected_rc" ] && [ "$rc" -ne "$expected_rc" ]; then
    fail "$name" "expected exit $expected_rc but got $rc"
    return
  fi
  # Normalize the compiled source path so tracebacks are stable across checkouts.
  local norm_out="$base.run.out.norm" norm_err="$base.run.err.norm"
  sed "s|$src|$(basename "$src")|g" "$base.run.out" >"$norm_out"
  sed "s|$src|$(basename "$src")|g" "$base.run.err" >"$norm_err"
  if [ -f "$stderrfile" ]; then
    if ! diff -u "$stderrfile" "$norm_err" >/dev/null 2>&1; then
      fail "$name" "stderr mismatch (rc=$rc)"
      diff -u "$stderrfile" "$norm_err" | head -40 || true
      return
    fi
  fi
  if [ -n "$expect" ]; then
    if diff -u "$expect" "$norm_out" >/dev/null 2>&1; then
      pass
    else
      fail "$name" "output mismatch (rc=$rc)"
      diff -u "$expect" "$norm_out" | head -40 || true
    fi
  else
    if [ $rc -eq 0 ] || [ -n "$expected_rc" ]; then pass; else fail "$name" "runtime exit $rc"; fi
  fi
}

echo "== Lightning test run =="
echo "Compiler: $LTNG"

# Category tests (Phase 0 infrastructure)
for t in "$ROOT"/tests/lexer/*.ltng "$ROOT"/tests/parser/*.ltng "$ROOT"/tests/semantic/*.ltng "$ROOT"/tests/runtime/*.ltng "$ROOT"/tests/codegen/*.ltng "$ROOT"/tests/e2e/*.ltng "$ROOT"/tests/native/*.ltng "$ROOT"/tests/regression/*.ltng "$ROOT"/tests/adversarial/*.ltng; do
  [ -f "$t" ] || continue
  rel="${t#$ROOT/tests/}"
  b="$(basename "$t")"
  exp="${t%.ltng}.expected"
  case "$rel" in
    codegen/*) mode="emit_c" ;;
    semantic/*neg*|parser/*neg*|lexer/*neg*) mode="check_fail" ;;
    *) mode="run" ;;
  esac
  if [ "$mode" = "run" ] && [ -f "$exp" ]; then
    run_case "$rel" "$t" "$exp" run
  elif [ "$mode" = "run" ]; then
    run_case "$rel" "$t" "" run
  else
    run_case "$rel" "$t" "" "$mode"
  fi
done

# Phase 0 smoke: existing regression corpus
for t in "$ROOT"/code/*.ltng; do
  b="$(basename "$t")"
  case "$b" in
    *err*|*errors*|t_lock_null.ltng|t_lock_type_change.ltng) continue;;
  esac
  exp="${t%.ltng}.expected"
  if [ -f "$exp" ]; then
    run_case "corpus/$b" "$t" "$exp" run
  else
    run_case "corpus/$b" "$t" "" run
  fi
done

# Error cases: must fail check/compile
for t in "$ROOT"/code/test_err_*.ltng "$ROOT"/code/*errors*.ltng "$ROOT"/code/tc_errors.ltng "$ROOT"/code/tc_if_errors.ltng "$ROOT"/code/t_lock_null.ltng "$ROOT"/code/t_lock_type_change.ltng; do
  [ -f "$t" ] || continue
  b="$(basename "$t")"
  # test_errors2.ltng is comment-only (documents error examples) and is a valid/empty program
  if [ "$b" = "test_errors2.ltng" ]; then
    continue
  fi
  run_case "negative/$(basename "$t")" "$t" "" check_fail
done



echo "---"
echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
if [ $FAIL -ne 0 ]; then exit 1; fi
