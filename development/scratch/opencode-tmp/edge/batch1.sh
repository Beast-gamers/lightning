#!/usr/bin/env bash
set -euo pipefail
R=/tmp/opencode/edge/run_case.sh

# closure in loop - shared mutable capture of loop var (numeric for)
"$R" clos_loop1 $'function make(){\n  let fs = 0\n}\n' "" 0 || true
