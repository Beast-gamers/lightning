#!/usr/bin/env bash
set -euo pipefail
# Build an UBSAN-instrumented compiler and run the full native test suite.
# ASAN may be unusable in some container/proot environments; UBSAN is lighter.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
COMPILER_DIR="$REPO_ROOT/source_code/compiler"
BIN="$REPO_ROOT/source_code/builds/bin/ltng"
BAK="/tmp/opencode/ltng_normal.bak.$(date +%s)"
mkdir -p /tmp/opencode

echo "==> Building UBSAN compiler"
gcc -std=c99 -O0 -g -fsanitize=undefined -fno-sanitize-recover=all \
  -o /tmp/opencode/ltng_ubsan \
  "$COMPILER_DIR"/source/src/main.c \
  "$COMPILER_DIR"/source/src/lexer.c \
  "$COMPILER_DIR"/source/src/parser_sym.c \
  "$COMPILER_DIR"/source/src/parser_expr.c \
  "$COMPILER_DIR"/source/src/parser_stmt.c \
  "$COMPILER_DIR"/source/src/codegen.c \
  "$COMPILER_DIR"/source/src/errors.c \
  "$COMPILER_DIR"/source/src/ir.c

cp "$BIN" "$BAK"
trap 'cp "$BAK" "$BIN" && touch "$BIN" && echo "==> Restored normal compiler"' EXIT
cp /tmp/opencode/ltng_ubsan "$BIN"
touch "$BIN"

echo "==> Running test suite under UBSAN"
UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
  bash "$ROOT/tests/run_tests.sh"
