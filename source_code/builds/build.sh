#!/usr/bin/env bash
set -euo pipefail

# Automated multi-architecture build for the Lightning (ltng) compiler.
# Builds per-arch binaries with zig (static musl), creates aliases, and
# regenerates/syncs universal wrappers.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPILER_DIR="$REPO_ROOT/source_code/compiler"
SRC_DIR="$COMPILER_DIR/source/src"
BIN_DIR="$SCRIPT_DIR/bin"
DIST_DIR="$SCRIPT_DIR/linux/compiler/dist"

# Source files (keep in sync with source_code/compiler/Makefile)
SRCS=(
  "$SRC_DIR/main.c"
  "$SRC_DIR/lexer.c"
  "$SRC_DIR/parser.c"
  "$SRC_DIR/codegen.c"
  "$SRC_DIR/errors.c"
  "$SRC_DIR/ir.c"
)

# zig targets: name|zig-triple|primary_output|aliases(comma-separated)
TARGETS=(
  "aarch64|aarch64-linux-musl|ltng_aarch64|ltng_arm64"
  "x86_64|x86_64-linux-musl|ltng_x86_64|ltng_amd64"
  "i686|x86-linux-musl|ltng_i686|ltng_x86"
  "armv7l|arm-linux-musleabihf|ltng_armv7l|"
  "riscv64|riscv64-linux-musl|ltng_riscv64|"
)

usage() {
  cat <<'EOF'
Usage: build.sh [--native-only] [--no-universal] [--targets list] [--help]

Builds Lightning compiler binaries for multiple architectures using zig and
syncs outputs into builds/bin and builds/linux/compiler/dist.

Options:
  --native-only     Build/sync only the host-native compiler (make).
  --no-universal    Skip regenerating the universal wrapper.
  --targets LIST    Comma-separated subset to build (names: aarch64,x86_64,i686,armv7l,riscv64).
  -h, --help        Show this help.

Environment:
  ZIG        zig executable (default: zig)
  ZIG_FLAGS  extra flags passed to `zig cc` (default: -O2 -std=c99 -static)
EOF
}

NATIVE_ONLY=0
NO_UNIVERSAL=0
TARGET_FILTER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --native-only) NATIVE_ONLY=1; shift ;;
    --no-universal) NO_UNIVERSAL=1; shift ;;
    --targets) TARGET_FILTER="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

ZIG_BIN="${ZIG:-zig}"
ZIG_FLAGS="${ZIG_FLAGS:--O2 -std=c99 -static}"

mkdir -p "$BIN_DIR" "$DIST_DIR" /tmp/opencode

for f in "${SRCS[@]}"; do
  if [ ! -f "$f" ]; then
    echo "Error: source file not found: $f" >&2
    exit 1
  fi
done

echo "==> Building native compiler (host)"
(cd "$COMPILER_DIR" && make clean >/dev/null 2>&1 || true; make)
if [ -f "$BIN_DIR/ltng" ]; then
  chmod 755 "$BIN_DIR/ltng" 2>/dev/null || true
  echo "    synced: $BIN_DIR/ltng"
fi

if [ "$NATIVE_ONLY" -eq 1 ]; then
  echo "==> Native-only build complete."
  exit 0
fi

if ! command -v "$ZIG_BIN" >/dev/null 2>&1; then
  echo "Error: zig not found (set ZIG=/path/to/zig)." >&2
  exit 1
fi

echo "==> Using zig: $($ZIG_BIN version 2>/dev/null || echo unknown)"

want_target() {
  local name="$1"
  [ -z "$TARGET_FILTER" ] && return 0
  case ",$TARGET_FILTER," in *",$name,"*) return 0 ;; *) return 1 ;; esac
}

TMP_DIR="$(mktemp -d /tmp/opencode/ltng-build.XXXXXX)"
cleanup_tmp() { rm -rf "$TMP_DIR"; }
trap cleanup_tmp EXIT

BUILT_PRIMARY=() # primary artifact names successfully built
FAILED=()

echo "==> Cross-compiling compiler binaries"
for entry in "${TARGETS[@]}"; do
  IFS='|' read -r name triple primary aliases <<<"$entry"
  if ! want_target "$name"; then
    continue
  fi
  out="$TMP_DIR/$primary"
  echo "  -> $name ($triple) => $primary"
  # shellcheck disable=SC2086
  if "$ZIG_BIN" cc -target "$triple" $ZIG_FLAGS -o "$out" "${SRCS[@]}"; then
    chmod 755 "$out" 2>/dev/null || true
    cp "$out" "$BIN_DIR/$primary"
    chmod 755 "$BIN_DIR/$primary" 2>/dev/null || true
    BUILT_PRIMARY+=("$primary")
    if [ -n "$aliases" ]; then
      IFS=',' read -r -a alias_arr <<<"$aliases"
      for a in "${alias_arr[@]}"; do
        [ -z "$a" ] && continue
        cp "$out" "$BIN_DIR/$a"
        chmod 755 "$BIN_DIR/$a" 2>/dev/null || true
        echo "     alias: $a"
      done
    fi
  else
    echo "     FAILED: $name" >&2
    FAILED+=("$name")
  fi
done

if [ "${#BUILT_PRIMARY[@]}" -eq 0 ]; then
  echo "Error: no cross-compilation targets built successfully." >&2
  exit 1
fi

if [ "$NO_UNIVERSAL" -eq 0 ]; then
  echo "==> Generating universal wrappers"
  python3 - "$BIN_DIR" "$DIST_DIR/ltng_universal" <<'PY'
import base64, os, pathlib, sys
bin_dir = pathlib.Path(sys.argv[1])
dist_out = pathlib.Path(sys.argv[2])
# All known arch artifact names (include aliases) in preferred detection order
arch_map = [
    ('x86_64', 'ltng_x86_64'),
    ('amd64', 'ltng_amd64'),
    ('aarch64', 'ltng_aarch64'),
    ('arm64', 'ltng_arm64'),
    ('i686', 'ltng_i686'),
    ('x86', 'ltng_x86'),
    ('armv7l', 'ltng_armv7l'),
    ('riscv64', 'ltng_riscv64'),
]
head = r'''#!/bin/sh
# Universal Lightning compiler executable
set -e
SELF="$0"
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    i686|i386|x86) ARCH="i686" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    armv7l|armv7*) ARCH="armv7l" ;;
    riscv64) ARCH="riscv64" ;;
esac
TMPBIN="${TMPDIR:-/tmp}/.ltng_univ_$$_${ARCH}_$(basename "$SELF")"
cleanup() { rm -f "$TMPBIN"; }
trap cleanup EXIT INT TERM
extract_and_run() {
    arch="$1"; shift
    marker="__LTNG_BIN_${arch}__"
    if ! grep -q "$marker" "$SELF" 2>/dev/null; then
        return 1
    fi
    awk -v m="$marker" '
        $0==m {found=1; next}
        found && $0==("__END_" m) {exit}
        found {print}
    ' "$SELF" | base64 -d > "$TMPBIN" 2>/dev/null || return 1
    chmod +x "$TMPBIN" 2>/dev/null || true
    "$TMPBIN" "$@"
    return $?
}
'''
tail = r'''extract_and_run "$ARCH" "$@" && exit $?
for a in x86_64 amd64 aarch64 arm64 i686 x86 armv7l riscv64; do
    [ "$a" = "$ARCH" ] && continue
    extract_and_run "$a" "$@" 2>/dev/null && exit $?
done
echo "Error: No compatible binary found for architecture $ARCH" >&2
exit 1
'''
parts = [head, tail]
embedded = []
for arch, fname in arch_map:
    p = bin_dir / fname
    if not p.exists():
        continue
    data = p.read_bytes()
    b64 = base64.b64encode(data).decode('ascii')
    b64 = '\n'.join(b64[i:i+76] for i in range(0, len(b64), 76))
    marker = f"__LTNG_BIN_{arch}__"
    parts.append(f"\n{marker}\n{b64}\n__END_{marker}\n")
    embedded.append(arch)
content = ''.join(parts)
for out in [bin_dir / 'ltng_universal', dist_out]:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(content)
    try:
        os.chmod(out, 0o755)
    except Exception:
        pass
    print(f"    wrote: {out} ({out.stat().st_size} bytes, embedded: {', '.join(embedded)})")
PY
fi

echo "==> Sync summary"
echo "    bin dir : $BIN_DIR"
echo "    dist    : $DIST_DIR/ltng_universal"
echo "    built   : ${BUILT_PRIMARY[*]}"
if [ "${#FAILED[@]}" -gt 0 ]; then
  echo "    failed  : ${FAILED[*]}" >&2
  exit 2
fi
echo "==> Done."
