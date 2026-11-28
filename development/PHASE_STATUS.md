# Lightning Phase Status

## Current state (latest verification)

- Full native test suite (latest): **PASS=228 FAIL=0 SKIP=0** (`bash development/tests/run_tests.sh`, compile+execute native binaries; includes existing corpus/regression, category/smoke, Phase 1/2/3/4/5 tests, and negative checks). Step 5 coverage: `development/tests/runtime/phase1_step5_inference.*`, `development/tests/runtime/phase1_step5_string_subexpr.*`, `development/tests/parser/phase1_step5_typeof_conv_neg.ltng`, `development/tests/parser/phase1_step5_typeof_interp_conv_neg.ltng`.
- Compiler/build interface: `bolt` is the user-facing interface; direct compiler invocation requires `LTNG_BOLT` (`Error: lightning compiler must be invoked through bolt`). `bolt --version` returns `1.0.0`; `bolt --emit-c` supported.
- Phase 1 Step 2 (type conversions/aliases) stabilization fixes landed and pushed (`f4f9c44`).
- Phase 1 Step 3 (division/modulo and overflow semantics) completed (incremental, pushed in slices): compile-time literal div/mod-by-zero rejection retained; runtime div/mod-by-zero throws `DivisionError: Division by zero` / `DivisionError: Modulo by zero` with traceback in covered contexts (including `let`/`set`, `write`, conditions, returns, call arguments, and for-loop step division for simple/common divisor forms) via runtime helpers/macros, parser-side divisor checks, and codegen guards/rewrites; integer variable/read parsing paths updated to 64-bit (`atoll`/`long long`) in covered numeric contexts; overflow behavior defined/documented and characterized by tests (C/`long long` emitted integer arithmetic and `double` float arithmetic, no explicit overflow trapping). See `development/PHASE1_PLAN.md`.
- Phase 1 Step 4 (distinct `int`/`float` semantics across paths) **completed** (verified): preserved typed arithmetic for locked/static numeric operands while keeping runtime dynamic `+`/concat for untyped/dynamic values; improved simple-return typing/inference for locked locals and coerced annotated `-> int`/`-> float` returns; `typeof` supports general expression arguments; fixed object property-access type inference/result typing. Additional Step 4 work: typed array element access for known homogeneous `int`/`float`/`bool` array elements in general expressions (arithmetic, comparisons, conditions, typeof, assignments/set contexts, nested access), comparison normalization for numeric expressions vs quoted numeric string literals, bare `write(arr[i])`/`write(arr[i][j]...)` special-case to preserve raw/OOB string output compatibility (multidim, bounded/dynamic emission), and for-each loop variable typing/locking based on inferred array element type to support typed iteration/accumulation in loop bodies. Added `development/tests/runtime/phase1_step4_array_elem.*`.
- **Phase 1 Step 5 (type inference and numeric promotion) complete (verified):** bool variable/element/expression typing preserved in covered inference paths; comparison/logical results remain `int`/`1`/`0` but now participate in numeric promotion in mixed expressions (e.g. `(1 < 2) + 0.5` infers/evaluates as `float`); chained immediate/indirect call results are treated as dynamic string results in covered expression/sole-call/assignment paths; string conditions (`if`/`while`/`do-while`, including dynamic/untyped simple variable conditions) use string-truthiness semantics consistent with `tobool` (`""`, `"0"`, `"f"`, `"false"` are false); `typeof`/interpolation resolves simple object field access, rejects obvious invalid literal `toint`/`tofloat`/alias conversions in covered fast paths, and correctly handles complex `typeof` expressions in direct and interpolation contexts (including conversion calls in larger arithmetic expressions and general interpolation expression inference with float-literal/dot disambiguation); prefix/postfix `++`/`--` use numeric semantics for `bool`/string/dynamic operands in covered contexts; conversion results (e.g. `toint`/`tobool`) participate correctly in mixed `int`/`float` promotion regardless of operand order (e.g. `1.5 + toint(3)`); scalar compound assignment RHS is parenthesized during expansion to preserve precedence with complex RHS expressions (e.g. ternary); array `.length` in expressions is emitted/typed numerically so equality/inequality comparisons (e.g. `a.length == n`) work correctly. **Ternary/conditional expressions (`cond ? a : b`) implemented** in covered contexts with branch/result type inference and numeric promotion. **String-concat subexpression fixes (final increment):** numeric `+`, `-`, `*`, `/`, `%` in parenthesized subexpressions within string-concat contexts now infer/emit correctly (including mixed int/float, nested expressions, comparisons/logical results, conversions, ternary branches), with type-aware add-vs-concat decisions and dynamic runtime dispatch where appropriate; numeric variables emit in numeric form for comparisons/arithmetic in string-concat subexpressions; ternary expressions in string-concat contexts produce correctly stringified results; logical `&&`/`||` emission avoids bitwise `&`/`|` miscompilation in comparison-chained conditions (e.g. `while (i < 3 && s < 100)`). Step 5 runtime tests: `development/tests/runtime/phase1_step5_inference.*`, `development/tests/runtime/phase1_step5_string_subexpr.*`; parser negatives retained. Full suite: **PASS=228 FAIL=0 SKIP=0**.
- **Phase 1 Steps 6–8 (completed, verified):** Step 6 tightened conservative type compatibility/operation checks (provably incompatible locked/dtype-fixed assignments including array/bool targets; string/char operands in `/` and `%`; unary numeric operators on string/char literals; provably non-numeric array indices including string/char/bool/null literals and locked non-numeric simple variables in expression/write/set index contexts). Step 7 tightened return validation (void-function value returns rejected; missing return on some control paths rejected for annotated non-void functions; annotated return type mismatches rejected for provable literal/locked-variable cases; `NODE_TRY` always-exits handling made sound for return-path validation; locked-variable returns validated against annotated function return types). Step 8 validation: added semantic negative coverage and kept full regression/native suite green at **PASS=228 FAIL=0 SKIP=0**. Memory tooling (valgrind/sanitizers) not available in this environment (recorded); multi-arch/universal dist binaries not rebuilt in this environment (native `source_code/builds/bin/ltng` updated).
- **Declaration locking semantics (verified):** value-initialized `let` declarations are now dtype-unlocked by default (dtype-mutable via `set`) across declaration forms; dtype-only declarations remain locked by default. Parameter symbols are now tracked (`is_param`) so typed inference/returns for known-typed unlocked locals are preserved without regressing dynamic/untyped parameter behavior. Array assignment typing/complexity audit (code + runtime helpers): element set is O(1) with bounds-checked in-place replacement; copy/slice/literal reassignment are O(n) in elements with per-element free/dup; insert/remove are O(n) due to shifting; push is amortized O(1); sort is O(n log n) (qsort); array element-type tracking is updated on element assignments and propagated on copies/slices/literal reassignments where applicable.
- **Step 4 final increment (verified):** default-typed parameters now drive body/expression typing without locking params or breaking `set`; call-result typing and pass-through call-site specialization cover direct/method/sole-call/argument paths, nested/chained calls, and first-class/indirect calls; `typeof` uses specialized call results and handles array elements in string interpolation. Array element assignment preserves homogeneous dtype (coercing RHS values), bool element push/insert/set emits valid string storage, compound element assignment is supported for numeric/string elements, negative indices use live length, OOB/string compatibility is covered across typed/string/condition contexts, deep (>3 indices) non-bare access is stack-safe, and invalid `typeof` array arity is rejected. Full suite after Step 5 completion: **PASS=228 FAIL=0 SKIP=0**.
- Runtime/generated-code string truncation fixed by moving emitted value/storage handling to dynamic strings/helpers (dynamic locals/params/returns, dynamic concat/repeat, dynamic formatting/return slots, and dynamic `read` input buffers), eliminating the reported 534-char truncation case in validated paths. Indirect-call dispatch now copies/mutates a local target buffer to avoid closure/function-value corruption on repeated calls.
- Compiler-internal limits made partially dynamic: `ErrorState` (errors/messages/source lines) and `Scope` variable storage now grow dynamically. Many compiler-internal fixed limits/buffers remain (e.g. token/identifier/AST/expression/output fixed-capacity fields and stack buffers in lexer/parser/codegen/IR paths) and require further refactoring for fully dynamic behavior.
- Multi-arch/universal compiler binaries and dist artifacts resynced for the latest source changes (`source_code/builds/bin/*`, `source_code/builds/linux/compiler/dist/ltng_universal`).

## Baseline (pre-Phase 0)

- Repository audited; `DEVELOPMENT_AUDIT.md` created.
- Baseline test run: **93 PASS, 0 FAIL, 0 SKIP** (existing regression corpus + negative checks, with `test_errors2.ltng` treated as valid comment-only program).

## Phase 0 — Testing infrastructure

**Status:** Complete (baseline infrastructure established)

### Implemented
- Created `tests/` structure: `lexer/`, `parser/`, `semantic/`, `runtime/`, `codegen/`, `e2e/`, `native/`, `regression/`, `adversarial/`, `helpers/`.
- Added `development/tests/run_tests.sh` reproducible test runner (builds compiler, copies to `/tmp/opencode/ltng`, runs category tests + existing corpus/regression + negative checks, compiles and executes native binaries, compares `.expected` outputs, supports `--check`/`--emit-c` modes).
- Created `TESTING.md` documenting test categories, how to run, conventions, and native-executable validation.
- Added initial category smoke tests: `tests/lexer/lexer_smoke.ltng`, `tests/parser/parser_smoke.ltng`, `tests/semantic/semantic_smoke.ltng`, `tests/runtime/runtime_smoke.ltng`, `tests/codegen/codegen_smoke.ltng` (emit-C), `tests/e2e/e2e_smoke.ltng`, `tests/native/native_smoke.ltng`, `tests/regression/regression_placeholder.ltng`.

### Test results
- Baseline infrastructure milestone: **PASS=101 FAIL=0 SKIP=0** (`bash tests/run_tests.sh` at that stage)
  - Category smokes: 8 passing (including emit-C check)
  - Existing corpus positive/runtime cases: 77 passing (with expected-output comparisons where present)
  - Existing negative/error cases: 16 passing (expected compile/check failures)
- Latest full suite (current): **PASS=161 FAIL=0 SKIP=0** (`bash development/tests/run_tests.sh`)

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

**Status:** Complete (Steps 1–8 implemented/verified; see notes for remaining hardening/known limitations)

### Implemented (so far)
- Made `detect_type` distinguish float literals/values as `TYPE_FLOAT` (instead of merging to int) and improved type name reporting (`float`, `char`) in covered paths.
- Improved expression/type handling for float literals/variables, numeric promotion in key mixed arithmetic cases, and codegen output for float-typed expressions.
- Added numeric compatibility in key assignment checks (`int <-> float`) and retained `char <-> string` compatibility in relevant cases to preserve backward-compatible behavior.
- Extended conversion-function recognition/handling for `toint`/`tofloat`/`tostring`/`tobool`/`tochar` and aliases `int`/`float`/`string`/`bool`/`char` across many expression, assignment, call/arg, return, condition/loop, comparison, interpolation/`typeof`, array, and nested/complex contexts; added stricter compile-time validation for obvious invalid literal numeric conversions in `toint`/`tofloat` (and aliases) in validated contexts.
- Step 2 stabilization fixes (follow-up, pushed `f4f9c44`): consistent `tobool` string semantics for `"t"/"f"/"true"/"false"/"0"/""` (including runtime/interpolation/complex paths), corrected bool↔int/float conversion emission, corrected `tostring`/`string` of bool-typed/compiled results to `"true"/"false"`, fixed C string escaping in conversion/expression emission, recognized `const` alias conversion-call initializers (`int/float/string/bool/char(...)`), and fixed bool-ternary comparison normalization (e.g. runtime `bool(var) == false` cases).
- Added compile-time detection/rejection for literal division/modulo by zero (int and float literal forms).
- Step 3 (completed): division/modulo and overflow semantics implemented/defined.
  - Compile-time literal div/mod-by-zero rejection retained.
  - Runtime div/mod-by-zero detection throws `DivisionError: Division by zero` / `DivisionError: Modulo by zero` with traceback in covered contexts/forms, including assignments/`let`/`set` numeric expressions, `write(...)` numeric expressions, conditions, returns, call arguments, and for-loop step division. Implemented via generated-runtime helpers/macros (`_ltng_div0_*`, `_LTNG_DIV_*`, `_LTNG_NZDIV_*`/`_LTNG_NZMOD_*`), parser-side divisor checks for simple numeric operands in collected expressions, and codegen guards/rewrites.
  - Division/modulo semantics: `int/int` truncates toward zero; mixed `int`/`float` division yields `float`; modulo supported in integer numeric paths (and guarded float-mod path where applicable).
  - Overflow behavior defined and characterized: integer arithmetic uses emitted C integer operations with `long long`/64-bit parsing (`atoll`) in updated integer variable/read paths and no explicit overflow trapping (signed overflow follows underlying C/platform behavior in practice, e.g. 64-bit wraparound in characterization tests); float arithmetic uses C `double`. Added characterization/runtime tests: `development/tests/runtime/phase1_div_zero_{arg,condition,return,runtime,write}.*`, `phase1_mod_zero_runtime.*`, `phase1_overflow_semantics.*` (and updated traceback positions for earlier runtime div/mod tests to operator/divisor locations).
- **Step 4 (completed):** distinct `int`/`float` semantics across key paths/categories, including final call-typing and array-element closure.
  - Expression/add typing: preserve typed C arithmetic for locked/static numeric operands while retaining runtime `_ltng_add_sn` (numeric-or-concat) dispatch for untyped/dynamic values (fixed `phase5_defaults` string-concat regression).
  - Return typing: simple `return <var>` preserves `int`/`float`/`bool` type for locked locals (improves inference/caller typing in covered cases); annotated `-> int`/`-> float` return values are coerced/formatted to match declared numeric semantics in emitted C.
  - `typeof`: supports general expression arguments (arithmetic/comparison/call/property expressions) and correctly handles function calls within larger expressions (e.g. `typeof(f()+1)`, `typeof(f()*2.0)`).
  - Object property access typing: expression result/type inference for `obj.field` now uses concrete field type (`int`/`float`/`bool`/`string`), fixing incorrect `typeof`/numeric typing for float and string fields in covered cases.
  - Float modulo/numeric emission: removed `sanitize_mod_atof` integer-cast rewriting so float `%` uses `fmod` runtime path correctly; added `-lm` to generated C compilation for math support.
  - Assignments/ops/conditions/interpolation/formatting: typed compound assignments (`+=`, `-=`, `*=`, `/=`), set/assignment type updates, mixed int/float arithmetic/promotion/comparisons, unary/`++`/`--`, array `.length` typing, and distinct int/float interpolation/printing verified in covered paths.
  - Array element typing (incremental): inferred homogeneous element types for array literals tracked in symbol table/AST and propagated through assignments/copies/slices and element/push/insert updates; known homogeneous `int`/`float`/`bool` element accesses are emitted as typed values in general expression contexts (supporting arithmetic, comparisons/conditions, typeof, and typed assignments in covered cases), comparison normalization accepts numeric expressions vs quoted numeric string literals, bare `write(arr[i])`/multidim element writes preserve raw/OOB string output compatibility, and for-each loop variables use/lock inferred element types for typed iteration/accumulation. Array runtime remains string-based (OOB reads still return empty string at runtime); typed numeric element access in non-bare contexts can change prior string/OOB observations for some cases and requires continued audit.
  - Added/extended `development/tests/runtime/phase1_step4_int_float.*`, `development/tests/runtime/phase1_step4_float_mod.*`, `development/tests/runtime/phase1_step4_mod_zero_float.*`, `development/tests/runtime/phase1_step4_paths.*`, `development/tests/runtime/phase1_step4_array_elem.*`.
- Improved return/expression typing for conversions and direct/indirect/named/self-referential function calls (fixed regressions causing incorrect string typing and internal compile failures in affected cases) and improved comparison normalization for numeric conversion helpers.
- Added support for explicit type declarations `let x = float` and `let x = char` (default initialization, distinct type tracking).
- Added/updated Phase 1 tests (runtime/semantic negative), including Step 2 conversion coverage and negative cases (e.g. `development/tests/runtime/phase1_conversions*.ltng`, `development/tests/semantic/phase1_*conversion*.ltng`, and updated corpus expectations where required).

### Test results
- Latest fully verified full suite: **PASS=228 FAIL=0 SKIP=0** (`bash development/tests/run_tests.sh`, native compile+execute validation).
- Step 3 completion: added/updated runtime and semantic Phase 1 coverage for division/modulo/overflow (`development/tests/runtime/phase1_div_zero_{arg,condition,return,runtime,write}.*`, `development/tests/runtime/phase1_mod_zero_runtime.*`, `development/tests/runtime/phase1_overflow_semantics.*`; updated stderr traces for earlier runtime div/mod tests). Phase 1 runtime and semantic negative checks remain passing in full-suite validation.
- Step 4 (final verification): full-suite re-verification complete (**PASS=177 FAIL=0 SKIP=0** at completion). Added/extended Step 4 runtime/parser coverage as recorded above.
- Step 5 (completed, verified): extended `development/tests/runtime/phase1_step5_inference.*`, added `development/tests/runtime/phase1_step5_string_subexpr.*` (string-concat subexpressions with numeric/complex/ternary/comparison operands and `&&`/`||` conditions), and retained parser negatives `development/tests/parser/phase1_step5_typeof_conv_neg.ltng`, `development/tests/parser/phase1_step5_typeof_interp_conv_neg.ltng`. Fixed numeric subexpressions (`+`, `-`, `*`, `/`, `%`, comparisons, conversions, ternary) in string-concat contexts, numeric variable emission in such subexpressions, ternary stringification in string-concat contexts, and logical `&&`/`||` emission for comparison-chained conditions. Full suite: **PASS=228 FAIL=0 SKIP=0**.
- Phase 1/Step 2 runtime and semantic checks continue to pass. Steps 4 and 5 are complete for covered paths; Step 6–8 work remains tracked in `development/PHASE1_PLAN.md`.

### Breaking/behavior changes (documented)
- `typeof(...)` reports `float` for float-typed values/literals in covered cases (instead of previous merged `int` behavior).
- Some previously-accepted faulty inputs are now rejected at compile time before C compilation (e.g. literal `x / 0`, `x % 0`, and obvious invalid literal numeric strings passed to `toint`/`tofloat` in validated contexts).

### Changed files (recent Phase 1 work, non-exhaustive)
- `source_code/compiler/source/src/parser.c`
- `source_code/compiler/source/src/codegen.c`
- `development/tests/run_tests.sh`
- `development/code/tc_arithmetic.ltng`, `development/code/test_tofunc3.*`, `development/code/test_typeconv.*` (as updated for conversion semantics)
- `development/tests/runtime/phase1_*.ltng` (+ `.expected`)
- `development/tests/semantic/phase1_*_neg.ltng`
- Rebuilt compiler artifacts (partial/latest): `source_code/builds/bin/ltng` (native) updated in recent Step 3/4 increments; `source_code/builds/bin/ltng*` (other arch/universal) and `source_code/builds/linux/compiler/dist/ltng_universal` may be stale pending sync.
- `CHANGELOG.md`, `PHASE_STATUS.md`, `development/TESTING.md`, `development/limitations.md`

### Remaining for Phase 1
- Complete and verify: distinct `int`/`float` semantics across **all** expression/codegen/runtime paths, full type inference coverage, consistent numeric promotion rules, comprehensive type compatibility checking, and complete return-type validation.
- Continue hardening Step 3 coverage as needed: runtime div/mod-by-zero guards cover common/simple divisor forms across major contexts but may miss some complex/nested divisor subexpressions or unusual emitted-expression forms; overflow policy is defined/characterized but not trapped.
- Continue expanding required positive/negative/boundary/interaction tests (precision, boundary values, invalid/runtime conversion behavior as applicable, mixed expressions, null behavior, and broader interaction/permutation cases) and record memory checks where available. Known observed edge: non-constant conversion expressions used as `switch case` labels may emit invalid C in some forms (conversions in `switch` expressions and constant case labels tested). Multi-arch/universal/dist artifacts remain unsynced for latest source changes.

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
- Fixed C compilation failure propagation in `source_code/compiler/src/main.c` to return non-zero when backend C compilation fails.
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
- `source_code/compiler/src/ltng.h` (added `NODE_BLOCK`)
- `source_code/compiler/src/parser.c`
- `source_code/compiler/src/codegen.c`
- `source_code/compiler/src/main.c`
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
- `source_code/compiler/src/ltng.h`
- `source_code/compiler/src/lexer.c`
- `source_code/compiler/src/parser.c`
- `source_code/compiler/src/codegen.c`
- `source_code/compiler/ltng` (rebuilt)
- `documentation.md`
- `CHANGELOG.md`
- `PHASE_STATUS.md`
- `tests/runtime/phase3_arrays.ltng`
- `tests/runtime/phase3_arrays.expected`
- `code/array_test.ltng`

### Notes / remaining limitations
- **Arrays of objects / jagged nested arrays:** not supported as first-class dynamic collections due to the current string-buffer/object runtime model; rectangular multidimensional arrays remain supported. (Likely requires broader runtime/object-model changes in later phases.)
- **`copy()` / `slice()` and array-returning expressions (partial):** `copy()`/`slice()` work in `let`/`set` and in general value-consumed expressions on temp arrays (scalar/formatted-string results). Function-returned arrays are supported via ah handle packing/unpacking, enabling assignment, return, and chaining (e.g. `getArray().sort()`). General array-typed params are supported when the body uses array ops/indexing/`for-in` on the param (ah pass/copy semantics), but arrays are still not fully first-class in all expression contexts.
- **Base-parameter UDMs on array receivers (partial):** supported when parser pre-scan detects array ops/indexing/`for-in` on `base`; uses ah handle pass/writeback. Functions with `base` not detected as array-used still treat `base` as string.
- **Element semantics:** values remain string-buffer formatted. `sort` now uses numeric-aware comparison when both compared elements are fully numeric (otherwise string comparison), improving numeric array sorting; `contains`/`index_of` still compare formatted string values. Full value/type semantics remain tied to the runtime/type-system model.
- **Memory validation:** runtime helpers free/replace elements on mutation/copy/slice paths, but no sanitizer/valgrind/leak-stress runs were recorded in this environment.
- **Dedicated aliasing (shared storage identity):** current array assignment/copy operations use copy semantics for safety in the existing ownership model; true shared-reference aliasing semantics would require deeper runtime/ownership redesign.

### Additional compiler pipeline update (post-Phase 3)

- **Implemented:** IR stage added after parsing/before C code generation (`source_code/compiler/src/ir.c`, integrated in `source_code/compiler/src/main.c`, build updated). Generates `<source basename>.ltng_ir` and cleans it up on successful non-debug operations; `--debug` preserves artifacts in `debug/<source basename>/` (`source.ltng`, `intermediete.ltng_ir`, `emitted.c` when available), captures timestamped/staged `logs.txt` (printed to console at end) and `compiler_log.txt` (best-effort Lightning source line to emitted C mapping), preserves real C compiler warnings/output in debug flows, and on successful native compilation produces a copied binary, debug-symbol binary (`.dbg`), object (`.o`), and assembly (`.s`) files.
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
- `source_code/compiler/src/ltng.h`
- `source_code/compiler/src/lexer.c`
- `source_code/compiler/src/parser.c`
- `source_code/compiler/src/codegen.c`
- `source_code/compiler/src/ir.c`
- `source_code/compiler/ltng` (rebuilt)
- `tests/run_tests.sh`
- `tests/runtime/phase4_*.ltng` + `.expected`/`.exitcode`/`.stderr`
- `tests/semantic/phase4_*_neg.ltng`
- `documentation.md`
- `CHANGELOG.md`
- `PHASE_STATUS.md`
- `limitations.md`

### Notes / remaining limitations
- Function-stack capture is bounded to 256 frames; very deep traces may omit older frames.
- Exception type/message/value/trace storage is heap-backed/dynamic (not fixed-size buffers), so very large payloads are limited by available memory rather than fixed buffer sizes.
- `exit` terminates the process without running pending `finally` blocks.
- No typed catch filtering (`catch (Type e)`); every `catch` handles all exceptions from its `try`.
- `finally` is implemented by inlining cleanup code at exit points; very deeply nested `try`/`finally` blocks increase generated code size.

## Phase 5 — Function system

**Status:** In progress

### Delivered increments
- **Default parameters:** `function f(a, b = 100) { ... }` — omitted call arguments use the declared default. Defaults may be numeric/string literals or expressions (including enclosing-scope references). Fill-missing logic applied in `parse_call_node` for all call paths (statement, expression, UFCS, nested call arguments). Tests: `tests/runtime/phase5_defaults.ltng`.
- **Named arguments:** calls support `name = value` arguments in any order, mixed with positional arguments (positional args bind left-to-right to parameters not supplied by name). Works with default parameters. Tests: `tests/runtime/phase5_named_args.ltng`.
- **Call argument validation:** compile-time errors for missing required arguments, too many arguments, unknown named parameters, and duplicate named arguments (with placeholder emission to avoid internal C errors). Tests: `tests/semantic/phase5_missing_arg_neg.ltng`, `tests/semantic/phase5_too_many_args_neg.ltng`, `tests/semantic/phase5_unknown_named_arg_neg.ltng`.
- **Return-type validation improvements:** compile-time error for clearly inconsistent return types within a function (e.g. string literal vs numeric), while remaining compatible with numeric mixing and current variable pass-through return representation. Tests: `tests/semantic/phase5_inconsistent_return_neg.ltng`.
- **Anonymous functions:** `function(params) { ... }` expressions can be assigned to `let`/`const` variables or reassigned with `set`, called, used in expressions/arguments, returned, and passed as values. Tests: `tests/runtime/phase5_anon_functions.ltng`.
- **Closures / shared mutable capture:** anonymous/nested functions capture enclosing-scope variables (parameters and locals) with **shared, mutable** semantics via heap-allocated storage; mutations are visible across the closure and creating scope, and captured state remains valid when closures escape. Tests: `tests/runtime/phase5_closures.ltng`, `tests/runtime/phase5_closure_mutable.ltng`.
- **Named function references as first-class values:** existing named functions can be referenced as values (e.g. `let f = add1`), assigned, passed, returned, and invoked through variables/parameters. Includes in-scope self-references (e.g. `let fn = fact` inside `fact`). Tests: `tests/runtime/phase5_named_func_refs.ltng`, `tests/runtime/phase5_nfr_self_ref.ltng`.
- **Function values / dynamic dispatch:** function values (closures and named function references) can be called through variables and parameters via runtime dispatch. Tests: `tests/runtime/phase5_indirect_call_in_expr.ltng`.
- **Immediate chained call syntax:** chained immediate invocation of call results is supported in general expression contexts, including sole RHS call contexts (`let`/`const`/`set`/`return`), conditions/comparisons, arithmetic expressions, `write(...)` arguments, and standalone call statements (e.g. `maker()(5)`, `a()(2)(3)`, `if (maker()(5) == 6)`, `write(1 + maker()(5))`).
- **Call-result typing/codegen fixes:** sole-call RHS type inference and numeric codegen now correctly handle inferred numeric function results (including arguments containing expressions), avoiding prior string-typed/invalid-C emission in affected cases.
- **Return-type annotations:** named functions may declare `-> type` return annotations (`int`, `float`, `string`, `char`, `bool`, `obj`, or `void`); return paths are validated (missing returns, void-return violations, type mismatches).

### Edge-case fixes (this pass)
- Fixed indirect/dynamic function-call emission in numeric-expression contexts (correct `__ltng_call_indirect` handling for callable-identified function values), preventing generated C/internal compile failures when function values are called inside expressions (e.g. `sum + fn(10)`).
- Fixed named function reference resolution for in-scope functions not yet linked in the AST (e.g. taking a self-reference inside the function body).
- Corrected array `slice(start, end)` runtime semantics to 1-based start with 1-based exclusive end (including negative-index handling), matching documented behavior and existing examples.
- Improved sole-call assignment/initialization typing and codegen for numeric call results and chained immediate calls in general expression contexts.
- Fixed string concatenation of call results: string-typed parts now use runtime string concatenation while numeric parts use numeric addition (`_ltng_add_sn`), and variadic call results in string concatenation are recognized via callee return-type inference; concatenation part buffers were enlarged to avoid truncating long statement expressions.

### Test results
- Full suite: **PASS=147 FAIL=0 SKIP=0** (`bash tests/run_tests.sh`, native compile+execute validation), including array slice coverage, mutable/shared capture, named function references/self-ref, indirect-call-in-expression, chained call forms, exception trace/buffer regression coverage, return annotations, variadics, and overloading.

### Remaining for Phase 5
- Variadic arguments: **implemented** this pass (trailing `...rest` parameter compiled to a dynamic array with hidden variadic ABI parameters; calls build/flatten rest arrays; `.length`/indexing supported). Negative case: variadic parameter must be last.
- Function overloading: **implemented** this pass (arity-based overload resolution, duplicate-signature rejection, ambiguous-call reporting). Negative cases: duplicate signatures, no matching overload, unknown/duplicate named arguments.
- Return-type checking/validation: **implemented** this pass for annotation/literal-based consistency (missing returns, void-return violations, mismatch errors); richer generic type annotations remain future work.

## Phases 6–13

Pending. Each phase will be updated with: implemented features, changed files, test results (positive/negative/boundary/interaction/regression/memory where applicable), discovered bugs, and remaining limitations. No phase marked complete until all completion criteria are satisfied.
