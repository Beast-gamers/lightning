# Lightning Testing Guide

## Overview

Testing infrastructure for the Lightning compiler/runtime. The goal is reproducible validation across compiler stages and native executable behavior.

- Compiler source: `compiler/src/*`
- Build: `cd compiler && make` (produces `compiler/ltng`)
- Test runner: `bash tests/run_tests.sh`

The test runner:
1. Builds the compiler from source.
2. Copies the compiler binary to `/tmp/opencode/ltng` and makes it executable (workaround for environments where workspace binaries may not be directly executable).
3. Runs regression, negative, and categorized tests, compiling to `/tmp/opencode/ltng_tests/*` and executing produced native binaries.
4. Reports `PASS/FAIL/SKIP` counts and exits non-zero on failure.

## Test categories

Directories under `tests/`:

- `tests/lexer/`: lexer/tokenization-focused cases (valid tokenization behavior, lexical errors where applicable).
- `tests/parser/`: parser/AST/syntax acceptance and rejection cases.
- `tests/semantic/`: semantic analysis checks (declarations, scopes, type/const rules, undefined names, etc.), primarily via `--check`.
- `tests/runtime/`: runtime behavior of compiled programs.
- `tests/codegen/`: code-generation checks (e.g. `--emit-c` produces C, selected structural/runtime equivalence checks).
- `tests/e2e/`: end-to-end source-to-executable scenarios.
- `tests/native/`: native executable execution/argument/exit-code behavior.
- `tests/regression/`: permanent regression cases for fixed bugs and preserved behavior.
- `tests/adversarial/`: fuzz-like, boundary, malformed, and interaction stress cases.

> Note: Phase 0 establishes the infrastructure and baseline regression coverage using the existing `code/*.ltng` corpus plus negative checks. Category-specific suites should be expanded continuously as features are implemented/fixed.

## How to run

### Full suite (baseline + categorized tests as added)

```bash
bash tests/run_tests.sh
```

### Individual program checks

```bash
# syntax/semantic check only
/tmp/opencode/ltng --check path/to/file.ltng

# compile and emit C
/tmp/opencode/ltng --emit-c path/to/file.ltng /tmp/out.c

# compile to native executable
/tmp/opencode/ltng path/to/file.ltng /tmp/out_bin

# run native executable
/tmp/out_bin [args...]

# compile-and-run via bolt (forwards args)
/path/to/bolt path/to/file.ltng [args...]
```

### Using an existing built compiler in-place

```bash
cd compiler && make
# then copy if needed:
cp compiler/ltng /tmp/opencode/ltng && chmod +x /tmp/opencode/ltng
```

## Baseline regression corpus

- Primary corpus: `code/*.ltng` (feature demos, tests, edge cases).
- Expected-output tests: paired `code/*.expected` files compared against program stdout.
- Negative/error tests: files expected to fail `--check`/compilation (e.g. `test_err_*`, most `*errors*`, `tc_*errors*`, selected lock/type error cases). `test_errors2.ltng` is comment-only and treated as a valid program.
- Examples: `examples/*.ltng` can be used for smoke/e2e validation.

## Adding tests

- Prefer small, focused tests named by feature/behavior.
- For positive runtime tests, include an `.expected` file (same basename) when deterministic stdout comparison is desired.
- For negative tests, ensure they fail with a non-zero compiler exit code; add comments describing the intended error.
- Add a regression test for every bug fixed (see bug-fixing protocol).
- When adding generated/native-executable tests, always compile and execute the produced binary (not just check/emit).

## Native executable validation

- A test is considered to validate native execution only when the compiler produces an executable and that executable is run with the intended inputs/arguments/environment.
- Verify exit codes where relevant (success and failure paths).
- Clean up temporary binaries in `/tmp/opencode`/test temp locations when appropriate.

## Memory and safety checks (when available)

Use available tooling on representative programs and generated executables, for example:

- C compiler sanitizers on emitted C / rebuilt artifacts where feasible (ASan/UBSan).
- Valgrind or similar leak/error detectors on native executables when installed.
- Fuzz/adversarial runs for lexer/parser robustness.

Record tool availability and results in `PHASE_STATUS.md` for each phase.

## Reproducibility notes

- Run tests from repository root unless noted.
- Keep temporary artifacts under `/tmp/opencode` (approved temp area) or test-managed temp paths.
- Do not depend on network access or external services for core compiler/runtime tests.
- Preserve existing tests and expected outputs; update them only when a documented, intentional behavior change is verified.
