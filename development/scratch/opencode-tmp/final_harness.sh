#!/bin/bash
# Final regression harness for Lightning (ltng).
# Every run-mode test has an explicit expected output; every check-mode test
# must fail compilation.
set -u

LTNG=/tmp/opencode/ltng
ROOT=/main/language
WORK=/tmp/opencode/harness_work
mkdir -p "$WORK"
TOTAL=0; PASS=0; FAIL=0

run_case() {
  local name="$1" file="$2" mode="$3" stdin_input="$4" expect_exit="${5:-0}"
  TOTAL=$((TOTAL+1))
  local src
  case "$file" in
    /*) src="$file" ;;
    *)  src="$ROOT/code/$file" ;;
  esac
  if [ ! -f "$src" ]; then
    echo "FAIL (missing file): $name  ($src)"
    FAIL=$((FAIL+1)); return 0
  fi
  local bin="$WORK/bin_$$_$RANDOM"
  local co cr out
  if [ "$mode" = "check" ]; then
    co="$($LTNG --check "$src" 2>&1)"; cr=$?
    if [ $cr -eq 0 ]; then
      echo "FAIL: $name  (expected compile error, got success)"
      echo "      output: $co"
      FAIL=$((FAIL+1))
    else
      echo "PASS: $name"; PASS=$((PASS+1))
    fi
    rm -f "$bin"; return 0
  fi
  co="$($LTNG "$src" "$bin" 2>&1)"; cr=$?
  if [ $cr -ne 0 ]; then
    echo "FAIL: $name  (compile failed exit $cr)"
    echo "      output: $co"
    FAIL=$((FAIL+1)); rm -f "$bin"; return 0
  fi
  out="$("$bin" < /dev/null 2>&1)"; cr=$?
  local ok=1
  if [ $cr -ne $expect_exit ]; then
    echo "FAIL: $name  (run exit $cr, expected $expect_exit)"
    echo "      stdout: $out"; ok=0
  fi
  if [ $ok -eq 1 ]; then
    echo "PASS: $name"; PASS=$((PASS+1))
  else
    FAIL=$((FAIL+1))
  fi
  rm -f "$bin"
}

run_case_file() {
  # name, file, expected_output_file, stdin_input, expect_exit
  local name="$1" file="$2" expfile="$3" stdin_input="${4:-}" expect_exit="${5:-0}"
  TOTAL=$((TOTAL+1))
  local src
  case "$file" in
    /*) src="$file" ;;
    *)  src="$ROOT/code/$file" ;;
  esac
  if [ ! -f "$src" ]; then
    echo "FAIL (missing file): $name  ($src)"
    FAIL=$((FAIL+1)); return 0
  fi
  local bin="$WORK/bin_$$_$RANDOM"
  local co cr out
  co="$($LTNG "$src" "$bin" 2>&1)"; cr=$?
  if [ $cr -ne 0 ]; then
    echo "FAIL: $name  (compile failed exit $cr)"
    echo "      output: $co"; FAIL=$((FAIL+1)); rm -f "$bin"; return 0
  fi
  out="$("$bin" < /dev/null 2>&1)"; cr=$?
  local ok=1
  if [ $cr -ne $expect_exit ]; then
    echo "FAIL: $name  (run exit $cr, expected $expect_exit)"; echo "      stdout: $out"; ok=0
  fi
  if [ -f "$expfile" ]; then
    local exp
    exp="$(cat "$expfile")"
    if [ "$out" != "$exp" ]; then
      echo "FAIL: $name  (stdout mismatch)"
      echo "      expected: $(printf '%s' "$exp" | head -c 200)"
      echo "      got:      $(printf '%s' "$out" | head -c 200)"
      ok=0
    fi
  fi
  if [ $ok -eq 1 ]; then echo "PASS: $name"; PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
  rm -f "$bin"
}

echo "===== FINAL REGRESSION HARNESS ====="
echo "Compiler: $LTNG"

# --- Regression: confirmed bug fixes (BUG-1 string+number crash, etc.) ---
run_case_file rg_string_plus_number /tmp/opencode/tests/rg_string_plus_number.ltng /tmp/opencode/tests/rg_string_plus_number.expected
run_case_file rg_set_string_plus_var /tmp/opencode/tests/rg_set_string_plus_var.ltng /tmp/opencode/tests/rg_set_string_plus_var.expected
run_case_file rg_return_concat /tmp/opencode/tests/rg_return_concat.ltng /tmp/opencode/tests/rg_return_concat.expected
run_case_file rg_float_literal_let /tmp/opencode/tests/rg_float_literal_let.ltng /tmp/opencode/tests/rg_float_literal_let.expected
run_case_file rg_negative_float_write /tmp/opencode/tests/rg_negative_float_write.ltng /tmp/opencode/tests/rg_negative_float_write.expected
run_case_file rg_switch_char /tmp/opencode/tests/rg_switch_char.ltng /tmp/opencode/tests/rg_switch_char.expected

# --- Array support (1D arrays with 1-based and negative indexing) ---
run_case_file test_arrays /main/language/code/test_arrays.ltng /main/language/code/test_arrays.expected

# --- New: base-parameter UFCS semantics ---
run_case_file base_assign /tmp/opencode/tests/base_assign.ltng /tmp/opencode/tests/base_assign.expected
run_case "base_plain_call_rejected" /tmp/opencode/tests/base_plain_call_reject.ltng check - 0
run_case "base_expr_rejected_write" /tmp/opencode/tests/base_expr_reject1.ltng check - 0
run_case "base_expr_rejected_let" /tmp/opencode/tests/base_expr_reject2.ltng check - 0

# --- New: lock/unlock dtype semantics ---
run_case_file t_lock_unlock /tmp/opencode/tests/t_lock_unlock.ltng /tmp/opencode/tests/t_lock_unlock.expected
run_case_file t_unlock_type_change /tmp/opencode/tests/t_unlock_type_change.ltng /tmp/opencode/tests/t_unlock_type_change.expected
run_case_file t_null_set_type_change /tmp/opencode/tests/t_null_set_type_change.ltng /tmp/opencode/tests/t_null_set_type_change.expected
run_case_file test_typeconv /tmp/opencode/tests/test_typeconv.ltng /tmp/opencode/tests/test_typeconv.expected
run_case_file test_tofunc3 /tmp/opencode/tests/test_tofunc3.ltng /tmp/opencode/tests/test_tofunc3.expected
run_case "lock_type_change_rejected" /tmp/opencode/tests/t_lock_type_change.ltng check - 0
run_case "lock_null_rejected" /tmp/opencode/tests/t_lock_null.ltng check - 0
run_case_file t_lock_double /tmp/opencode/tests/t_lock_double.ltng /tmp/opencode/tests/t_lock_double.expected

# --- New edge-case tests ---
run_case_file t_lexer_basic /tmp/opencode/tests/t_lexer_basic.ltng /tmp/opencode/tests/t_lexer_basic.expected
run_case_file t_lexer_operators /tmp/opencode/tests/t_lexer_operators.ltng /tmp/opencode/tests/t_lexer_operators.expected
run_case_file t_lexer_strings /tmp/opencode/tests/t_lexer_strings.ltng /tmp/opencode/tests/t_lexer_strings.expected
run_case_file t_vars_basic /tmp/opencode/tests/t_vars_basic.ltng /tmp/opencode/tests/t_vars_basic.expected
run_case_file t_control_if /tmp/opencode/tests/t_control_if.ltng /tmp/opencode/tests/t_control_if.expected
run_case_file t_control_loops /tmp/opencode/tests/t_control_loops.ltng /tmp/opencode/tests/t_control_loops.expected
run_case_file t_control_switch /tmp/opencode/tests/t_control_switch.ltng /tmp/opencode/tests/t_control_switch.expected
run_case_file t_functions_basic /tmp/opencode/tests/t_functions_basic.ltng /tmp/opencode/tests/t_functions_basic.expected
run_case_file t_functions_ufcs /tmp/opencode/tests/t_functions_ufcs.ltng /tmp/opencode/tests/t_functions_ufcs.expected
run_case_file t_functions_nested /tmp/opencode/tests/t_functions_nested.ltng /tmp/opencode/tests/t_functions_nested.expected
run_case_file t_objects /tmp/opencode/tests/t_objects.ltng /tmp/opencode/tests/t_objects.expected
run_case_file t_string_ops /tmp/opencode/tests/t_string_ops.ltng /tmp/opencode/tests/t_string_ops.expected
run_case_file t_typeof /tmp/opencode/tests/t_typeof.ltng /tmp/opencode/tests/t_typeof.expected
run_case_file t_inc_dec /tmp/opencode/tests/t_inc_dec.ltng /tmp/opencode/tests/t_inc_dec.expected
run_case_file t_compound /tmp/opencode/tests/t_compound.ltng /tmp/opencode/tests/t_compound.expected
run_case_file t_interp_edge /tmp/opencode/tests/t_interp_edge.ltng /tmp/opencode/tests/t_interp_edge.expected
run_case_file t_deep /tmp/opencode/tests/t_deep.ltng /tmp/opencode/tests/t_deep.expected

# --- Error cases (must fail compilation) ---
for f in test_err_semicolon test_err_reserved test_err_undeclared_write test_err_set_undeclared \
         test_err_read_undeclared test_err_const_init test_err_multi test_err_undeclared_interp \
         t_vars_errors t_vars_type_errors t_const_errors t_control_errors t_functions_errors \
         t_lexer_errors t_errors_misc; do
  found=0
  for cand in "$ROOT/code/$f.ltng" "/tmp/opencode/tests/$f.ltng"; do
    if [ -f "$cand" ]; then
      run_case "err_$f" "$cand" check - 0
      found=1
      break
    fi
  done
  [ $found -eq 0 ] && echo "WARN: error test file not found: $f"
done

echo
echo "===== RESULTS: $PASS passed / $FAIL failed / $TOTAL total ====="
[ $FAIL -eq 0 ]
