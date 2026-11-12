# Lightning Phase Status

## Baseline (pre-Phase 0)

- Repository audited; `DEVELOPMENT_AUDIT.md` created.
- Baseline test run via `tests/run_tests.sh`: **93 PASS, 0 FAIL, 0 SKIP** (existing `code/` regression corpus + negative checks, with `test_errors2.ltng` treated as valid comment-only program).

## Phase 0 — Testing infrastructure

**Status:** Complete (baseline infrastructure established)

### Implemented
- Created `tests/` structure: `lexer/`, `parser/`, `semantic/`, `runtime/`, `codegen/`, `e2e/`, `native/`, `regression/`, `adversarial/`, `helpers/`.
- Added `tests/run_tests.sh` reproducible test runner (builds compiler, copies to `/tmp/opencode/ltng`, runs category tests + existing `code/` regression corpus + negative checks, compiles and executes native binaries, compares `.expected` outputs, supports `--check`/`--emit-c` modes).
- Created `TESTING.md` documenting test categories, how to run, conventions, and native-executable validation.
- Added initial category smoke tests: `tests/lexer/lexer_smoke.ltng`, `tests/parser/parser_smoke.ltng`, `tests/semantic/semantic_smoke.ltng`, `tests/runtime/runtime_smoke.ltng`, `tests/codegen/codegen_smoke.ltng` (emit-C), `tests/e2e/e2e_smoke.ltng`, `tests/native/native_smoke.ltng`, `tests/regression/regression_placeholder.ltng`.

### Test results
- **PASS=101 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`)
  - Category smokes: 8 passing (including emit-C check)
  - Existing corpus positive/runtime cases: 77 passing (with expected-output comparisons where present)
  - Existing negative/error cases: 16 passing (expected compile/check failures)

### Changed files
- `DEVELOPMENT_AUDIT.md` (new)
- `TESTING.md` (new)
- `PHASE_STATUS.md` (new)
- `tests/run_tests.sh` (new)
- `tests/lexer/lexer_smoke.ltng` (new)
- `tests/parser/parser_smoke.ltng` (new)
- `tests/semantic/semantic_smoke.ltng` (new)
- `tests/runtime/runtime_smoke.ltng` (new)
- `tests/codegen/codegen_smoke.ltng` (new)
- `tests/e2e/e2e_smoke.ltng` (new)
- `tests/native/native_smoke.ltng` (new)
- `tests/regression/regression_placeholder.ltng` (new)

### Known issues / limitations
- Compiler stages are not exposed as separate unit-testable libraries; stage-specific tests are expressed via `--check`, `--emit-c`, and compile+run of native executables.
- Sanitizer/fuzz tooling availability varies by environment; to be used and recorded when available.
- `test_errors2.ltng` is comment-only (valid program) and excluded from negative classification.

### Notes
- Existing user changes/untracked files preserved (e.g. `code/compiled/`).
- Compiler build produces warnings (format truncation, unused function, switch handling) to be addressed in later phases as appropriate.

## Phase 1 — Type system

**Status:** Partially implemented (core improvements landed; further work required)

### Implemented (so far)
- Made `detect_type` distinguish float literals/values as `TYPE_FLOAT` (instead of merging to int).
- Updated type name reporting (`var_type_name`, `type_name_str`) to return distinct `float` and `char` type names.
- Improved expression handling to recognize float literals/variables as float in type tracking and promote numeric results to float in key cases.
- Added numeric compatibility in assignment type checks (`int <-> float`) and `char <-> string` compatibility to preserve backward-compatible behavior while enabling distinct type tracking.
- Extended conversion-function recognition to include `tofloat` and `tochar` (in addition to existing `toint`/`tostring`/`tobool`) and added initial parser/expression handling for them; updated `typeof(tofloat(...))`/`typeof(tochar(...))` mapping in key paths.
- Added support for explicit type declarations `let x = float` and `let x = char` (default initialization, distinct type tracking).
- Added `tests/runtime/phase1_types_smoke.ltng` for basic distinct-type/typeof/conversion behavior.

### Test results
- Full suite after changes: **PASS=101 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`)
- New smoke test: `tests/runtime/phase1_types_smoke.ltng` passes.

### Breaking/behavior changes (documented)
- `typeof(...)` now reports `float` for float-typed values/literals in covered cases (instead of merged `int`).
- Updated existing expected output: `code/t_null_set_type_change.expected` changed final typeof from `int` to `float` for `3.14`.

### Changed files
- `compiler/src/parser.c`
- `code/t_null_set_type_change.expected`
- `tests/runtime/phase1_types_smoke.ltng`
- `CHANGELOG.md`
- `PHASE_STATUS.md`

### Remaining for Phase 1
- Complete and verify: distinct int/float semantics across all expression/codegen paths, full type inference, explicit conversions coverage (`tofloat`/`tochar` in all contexts including interpolation/evaluation paths), numeric promotion rules, comprehensive type compatibility checking, return-type validation, overflow and division semantics.
- Add required positive/negative/boundary/interaction tests (precision, boundary values, invalid conversions, mixed expressions, null behavior) and memory checks where available.

## Phase 2 — Semantic analysis and scopes

**Status:** Complete (implemented and regression-tested)

### Implemented
- Relaxed declaration restrictions to allow `let`/`const` declarations in control-flow blocks, nested blocks, and function contexts (removed `disallow_decl` enforcement in parser paths).
- Enabled variable **shadowing** by changing declaration duplicate detection to same-scope checks (`symbol_table_has_local`) and resolving references to the innermost visible declaration.
- Added scope-aware C naming for shadowed declarations (unique emitted C identifiers for shadowing locals) while preserving existing names for non-shadowing cases to maintain compatibility.
- Updated parser/codegen to support block-scoped local declarations and lifetimes in generated C:
  - inline declaration emission for block/function-local `let`/`const` (instead of incorrect broad hoisting)
  - block-local declarations in `if`/`else`, `while`, `do-while`, `for`, `switch` case blocks, and nested blocks
  - correct handling of shadowed variable reads/writes/sets and loop variable scoping
- Fixed C compilation failure propagation in `compiler/src/main.c` to return non-zero when backend C compilation fails.
- Fixed object-literal handling for `set` assignments (emit assignment/update semantics instead of redeclaration) to remain compatible with inline declaration changes.

### Verified
- Declarations in loops/conditionals/switch cases/nested blocks: supported and tested.
- Shadowing: inner declarations shadow outer variables and outer values remain intact after block exit.
- Same-scope duplicate declarations: rejected.
- Undefined variable usage: rejected.
- Constant reassignment: rejected.
- Existing language behavior: preserved across full regression suite.

### Test results
- Full suite: **PASS=109 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`)
- New Phase 2 tests:
  - `tests/semantic/phase2_scope_block_let.ltng` (pass)
  - `tests/semantic/phase2_shadowing.ltng` (pass)
  - `tests/semantic/phase2_nested_blocks.ltng` (pass)
  - `tests/semantic/phase2_switch_decl.ltng` (pass)
  - `tests/semantic/phase2_duplicate_same_scope_neg.ltng` (expected fail, pass)
  - `tests/semantic/phase2_const_enforcement_neg.ltng` (expected fail, pass)
  - `tests/semantic/phase2_undefined_neg.ltng` (expected fail, pass)

### Changed files
- `compiler/src/ltng.h`
- `compiler/src/parser.c`
- `compiler/src/codegen.c`
- `compiler/src/main.c`
- `tests/semantic/phase2_scope_block_let.ltng`
- `tests/semantic/phase2_shadowing.ltng`
- `tests/semantic/phase2_nested_blocks.ltng`
- `tests/semantic/phase2_switch_decl.ltng`
- `tests/semantic/phase2_duplicate_same_scope_neg.ltng`
- `tests/semantic/phase2_const_enforcement_neg.ltng`
- `tests/semantic/phase2_undefined_neg.ltng`
- `PHASE_STATUS.md`
- `CHANGELOG.md`

### Notes / limitations
- Bare `{}` blocks (not attached to control constructs/functions) are not part of current language syntax; nested scoping is verified via supported block constructs.
- Scope/lifetime semantics are implemented for the current string-buffer runtime model; further hardening may be needed as later phases change runtime representations.

## Phases 3–13

Pending. Each phase will be updated with: implemented features, changed files, test results (positive/negative/boundary/interaction/regression/memory where applicable), discovered bugs, and remaining limitations. No phase marked complete until all completion criteria are satisfied.
