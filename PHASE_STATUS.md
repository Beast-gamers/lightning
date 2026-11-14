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
- Declarations in loops/conditionals/switch cases/nested blocks and standalone bare `{ ... }` blocks: supported and tested.
- Shadowing: inner declarations shadow outer variables and outer values remain intact after block exit.
- Same-scope duplicate declarations: rejected.
- Undefined variable usage: rejected.
- Constant reassignment: rejected.
- Existing language behavior: preserved across full regression suite.

### Test results
- Full suite: **PASS=111 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`)
- Phase 2 semantic/runtime checks continue to pass (including block scope, nested scopes, shadowing, switch-case declarations, duplicate/const/undefined negatives).

### Changed files
- `compiler/src/ltng.h` (added `NODE_BLOCK`)
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
- Scope/lifetime semantics are implemented for the current string-buffer runtime model; further hardening may be needed as later phases change runtime representations.

## Phase 3 — Arrays and collections

**Status:** Complete (implemented and regression-tested; key limitations reduced)

### Implemented
- **Dynamic storage:** arrays use `char **data` + `int len` + `int cap` with automatic growth/reallocation (replaces fixed-size/static patterns in emitted code paths).
- **Runtime helpers:** `_ltng_arr_new`, `_ltng_arr_ensure`, `_ltng_arr_set_elem`, `_ltng_arr_get`, `_ltng_arr_set`, `_ltng_arr_push`, `_ltng_arr_pop`, `_ltng_arr_pop_index`, `_ltng_arr_insert`, `_ltng_arr_remove`, `_ltng_arr_contains`, `_ltng_arr_index_of`, `_ltng_arr_reverse`, `_ltng_arr_sort`, `_ltng_arr_copy`, `_ltng_arr_slice`, `_ltng_arr_free`, `_ltng_arr_strdup`.
- **Indexing semantics preserved:** 1-based positive indices, negative indices count from end, index `0` invalid; out-of-bounds/invalid reads return empty string (existing behavior preserved).
- **Core API:** `get`/`set` via indexing (`arr[i]`, `set arr[i]=...`), `length`, `push(value)`, `pop()`, `pop(index)`, `insert(index, value)`, `remove(index)`, `contains(value)`, `index_of(value)`, `sort()`, `reverse()`.
- **Copy/slice/assignment:** `let b = a`, `let b = a.copy()`, `let b = a.slice(start, end)`, and `set b = a`, `set b = a.copy()`, `set b = a.slice(start, end)` for existing array variables (1-based start, exclusive end; negative indices supported).
- **Iteration:** index-based iteration via `length`/loops and **for-each** syntax `for (x in arr) { ... }`.
- **Usage contexts:** array methods/properties supported in statements and in `write(...)` expressions where applicable; `.length` usable in numeric comparisons/conditions and `for` ranges.
- **Bug fixes (discovered during validation):** added missing `_ltng_arr_set` runtime helper for array element assignment; fixed `_ltng_arr_copy`/`_ltng_arr_slice` cleanup to avoid leaks/double-frees when replacing array contents.
- **Compatibility:** existing multidimensional array behavior and prior `test_arrays` corpus preserved.

### Test results
- Full suite: **PASS=111 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`, native compile+execute validation).
- Existing regression: `code/test_arrays.ltng` passes with dynamic array backend.
- Phase 3/array coverage: `tests/runtime/phase3_arrays.ltng` (+ `.expected`), `code/array_test.ltng` (comprehensive native test including copy/slice in `set`, for-each, edge cases).

### Changed files
- `compiler/src/ltng.h`
- `compiler/src/lexer.c`
- `compiler/src/parser.c`
- `compiler/src/codegen.c`
- `compiler/ltng` (rebuilt)
- `documentation.md`
- `CHANGELOG.md`
- `PHASE_STATUS.md`
- `tests/runtime/phase3_arrays.ltng`
- `tests/runtime/phase3_arrays.expected`
- `code/array_test.ltng`

### Notes / remaining limitations
- **Arrays of objects / jagged nested arrays:** not supported as first-class dynamic collections due to the current string-buffer/object runtime model; rectangular multidimensional arrays remain supported. (Likely requires broader runtime/object-model changes in later phases.)
- **`copy()` / `slice()` as general array-producing expressions:** supported in `let`/`set` array assignment forms, but not in all expression contexts (arrays are not first-class expression values in the current runtime).
- **Element semantics:** values remain string-buffer formatted. `sort` now uses numeric-aware comparison when both compared elements are fully numeric (otherwise string comparison), improving numeric array sorting; `contains`/`index_of` still compare formatted string values. Full value/type semantics remain tied to the runtime/type-system model.
- **Memory validation:** runtime helpers free/replace elements on mutation/copy/slice paths, but no sanitizer/valgrind/leak-stress runs were recorded in this environment.
- **Dedicated aliasing (shared storage identity):** current array assignment/copy operations use copy semantics for safety in the existing ownership model; true shared-reference aliasing semantics would require deeper runtime/ownership redesign.

### Additional compiler pipeline update (post-Phase 3)

- **Implemented:** IR stage added after parsing/before C code generation (`compiler/src/ir.c`, integrated in `compiler/src/main.c`, build updated). Generates `<source basename>.ltng_ir` and cleans it up on successful non-debug operations; `--debug` preserves artifacts in `debug/<source basename>/` (`source.ltng`, `intermediete.ltng_ir`, `emitted.c` when available), captures timestamped/staged `logs.txt` (printed to console at end) and `compiler_log.txt` (best-effort Lightning source line to emitted C mapping), preserves real C compiler warnings/output in debug flows, and on successful native compilation produces a copied binary, debug-symbol binary (`.dbg`), object (`.o`), and assembly (`.s`) files.
- **Verification:** targeted manual checks for normal compile, `--check`, `--emit-c`, and `--debug` (including debug artifacts, logs, compiler_log mapping, binary/obj/asm/dbg); full suite `PASS=111 FAIL=0 SKIP=0`.

## Phase 4 — Exception handling

**Status:** Complete (implemented and regression-tested)

### Implemented
- **Syntax:** `try { ... } catch (e) { ... } finally { ... }`, `catch e { ... }` without parentheses, optional catch variable, and `finally` with or without `catch`. `try` requires at least one of `catch`/`finally`.
- **Throwing:** `throw <expression>` accepts strings, numbers, booleans, variables, and expressions; `throw error(type, message)` sets an explicit type/message; `throw CustomError("message")` (identifier ending in `Error`, when not a user-defined function) sets a custom type.
- **Rethrow:** bare `throw` inside a `catch` rethrows the active exception; bare `throw` outside `catch` is a compile-time error.
- **Catch objects:** catch variables expose `type`, `message`, `trace`, `line`, `column`, `function`, `value`, and `obj`. Catch variables are block-scoped, and nested catches with the same source name use unique emitted C names.
- **Propagation:** exceptions propagate through nested `try` blocks and function calls to the nearest handler.
- **Uncaught exceptions:** print `<Type>: <message>` followed by a stack trace to stderr, then `exit(1)`.
- **Stack traces:** call-site-aware frames (`at <function> (<file>:<line>:<col>)`), with the first frame at the throw site and each subsequent frame at the caller's call site.
- **Stack traces:** call-site-aware frames (`at <function> (<file>:<line>:<col>)`), with the first frame at the throw site and each subsequent frame at the caller's call site.
- **`else` success block:** optional `else` clause after `catch` runs only when the `try` body completes normally; skipped on `throw`/`return`/`break`/`continue`; requires a `catch`; runs before `finally`; exceptions thrown in `else` propagate outward (not caught by the same catch) while `finally` still runs.
- **`finally` semantics:** runs on normal completion, `throw`, `return`, `break`, and `continue`; `return` in `finally` overrides a pending return/exception; a new exception thrown in `finally` replaces a pending one; `break`/`continue` in `finally` leave the loop after cleanup.
- **Runtime:** `setjmp`/`longjmp` try-frame chain, active exception state, stack-local saved-exception capture for rethrow, and a bounded function stack for trace capture.

### Test results
- Full suite: **PASS=125 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`, native compile+execute validation).
- Phase 4 positive/native tests: `tests/runtime/phase4_try_catch`, `phase4_try_else`, `phase4_try_else_flow`, `phase4_custom_errors`, `phase4_finally_control`, `phase4_finally_throws`, `phase4_nested_rethrow`, `phase4_propagation`, `phase4_uncaught`, `phase4_catch_scope`, `phase4_finally_exit_exc` (with `.expected`, and `.exitcode`/`.stderr` where applicable).
- Phase 4 negative tests: `tests/semantic/phase4_bare_throw_neg`, `phase4_catch_without_try_neg`, `phase4_finally_without_try_neg`, `phase4_throw_missing_message_neg`, `phase4_try_without_clause_neg`, `phase4_try_else_without_catch_neg`, `phase4_try_else_after_finally_neg`.
- **UBSan:** all Phase 4 programs compiled with `-fsanitize=undefined -fno-sanitize-recover=all` and executed cleanly (9/9; `phase4_uncaught` exits 1 as expected).
- **ASan/valgrind:** ASan could not run in this proot environment (`heap size ... exceeds max user virtual address`); valgrind is unavailable. Memory checks are therefore limited to UBSan plus native execution.

### Bugs found and fixed during Phase 4
- Pending-exception save/restore leak when `finally` exited via `return`/`break`/`continue` on the exception path; replaced the global depth-indexed saved-exception stack with stack-local `LtngExcSaved` captures.
- `throw true` / `throw false` were emitted as internal numeric values; now preserved as `"true"`/`"false"`.
- Stack traces initially reported the throw site for every frame; added hidden call-site line/column arguments so each caller frame reports its own call site.
- Nested catch variables with the same source name could collide; catch variables now always receive unique emitted C names and schemas are registered under those names.

### Changed files
- `compiler/src/ltng.h`
- `compiler/src/lexer.c`
- `compiler/src/parser.c`
- `compiler/src/codegen.c`
- `compiler/src/ir.c`
- `compiler/ltng` (rebuilt)
- `tests/run_tests.sh`
- `tests/runtime/phase4_*.ltng` + `.expected`/`.exitcode`/`.stderr`
- `tests/semantic/phase4_*_neg.ltng`
- `documentation.md`
- `CHANGELOG.md`
- `PHASE_STATUS.md`
- `limitations.md`

### Notes / remaining limitations
- Trace text is bounded (`_ltng_exc_trace[8192]`); very deep traces may be truncated.
- Function-stack capture is bounded to 256 frames.
- Exception type/message/value/trace storage uses fixed buffers, so very large payloads are truncated.
- `exit` terminates the process without running pending `finally` blocks.
- No typed catch filtering (`catch (Type e)`); every `catch` handles all exceptions from its `try`.
- `finally` is implemented by inlining cleanup code at exit points; very deeply nested `try`/`finally` blocks increase generated code size.

## Phase 5 — Function system

**Status:** In progress (first increment landed: default parameters)

### Implemented so far
- **Default parameters:** `function f(a, b = 100) { ... }` — omitted call arguments use the declared default. Defaults may be numeric or string literals. Fill-missing logic applied in `parse_call_node` for all call paths (statement, expression, UFCS, nested call arguments).

### Test results
- Full suite: **PASS=130 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`, native compile+execute validation).
- New: `tests/runtime/phase5_defaults.ltng` (+ `.expected`) covering numeric/string defaults and multiple defaults.

### Remaining for Phase 5
- Anonymous functions and closures (capture of enclosing function locals, captured mutable variables, independent instances).
- Function references / first-class function values.
- Named arguments.
- Variadic arguments.
- Function overloading.
- Return-type checking/validation.

## Phases 6–13

Pending. Each phase will be updated with: implemented features, changed files, test results (positive/negative/boundary/interaction/regression/memory where applicable), discovered bugs, and remaining limitations. No phase marked complete until all completion criteria are satisfied.
