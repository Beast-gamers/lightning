#!/usr/bin/env bash
set -euo pipefail
name="$1"; src="$2"; expect_out="${3-}"; expect_rc="${4-0}"
dir=/tmp/opencode/edge
bin="$dir/$name"
rm -f "$bin" "$bin.out" "$bin.err"
printf '%s' "$src" > "$dir/$name.ltng"
if ! /tmp/opencode/ltng "$dir/$name.ltng" "$bin" >"$bin.cout" 2>"$bin.cerr"; then
  echo "COMPILE_FAIL $name"
  cat "$bin.cerr" 2>/dev/null | head -20
  exit 10
fi
set +e
"$bin" >"$bin.out" 2>"$bin.err"
rc=$?
set -e
if [ "$rc" -ne "$expect_rc" ]; then
  echo "RC_FAIL $name expected=$expect_rc got=$rc"
  echo "---out---"; cat "$bin.out"; echo "---err---"; cat "$bin.err"
  exit 11
fi
if [ -n "$expect_out" ]; then
  if [ "$(cat "$bin.out")" != "$expect_out" ]; then
    echo "OUT_FAIL $name"
    echo "---expected---"; printf '%s\n' "$expect_out"
    echo "---got---"; cat "$bin.out"
    echo "---err---"; cat "$bin.err"
    exit 12
  fi
fi
echo "OK $name"
