# Phase 1 Implementation Plan — Type System

## 1. Scope and objectives

Phase 1 (per `development/promt.md`) requires implementing and stabilizing:

- Distinct `int` and `float` types
- `string`, `char`, `bool`, `null`, `array`, and `object` types
- Type inference
- Explicit conversions
- Numeric promotion
- Type compatibility checking
- Return-type validation
- Overflow and division semantics

Required validation focus: precision, boundary values, invalid conversions, mixed expressions, and null behavior.

**Completion bar (Phase 1):** implementation works; existing features still work; positive/negative/boundary/interaction tests pass; regression tests pass; memory checks pass where available; native executables behave correctly; docs updated; no known critical/high-severity defects remain.

## 2. Current baseline (as of this plan)

- Phase 0–4 complete; Phase 5 in progress (separate track). Phase 1 is **partially implemented**.
- Latest fully verified full native suite: **PASS=177 FAIL=0 SKIP=0** (`bash development/tests/run_tests.sh`, compile+execute native binaries). Native compiler binary updated; multi-arch/universal and dist artifacts resynced for the latest source changes.
- Progress landed:
  - **Step 1 (early type-system work):** distinct `float` detection/reporting in key paths, `typeof` improvements, float expression handling/output in covered cases, numeric compatibility in key assignments (`int <-> float`), `char <-> string` compatibility in relevant cases, explicit `let x = float` / `let x = char` declarations, compile-time rejection of literal div/mod by zero and obvious invalid literal numeric strings for `toint`/`tofloat` in validated contexts, initial Phase 1 runtime/negative tests.
  - **Step 2 (conversions/aliases stabilization):** `toint`/`tofloat`/`tostring`/`tobool`/`tochar` and aliases `int/float/string/bool/char` handling across many contexts; `tobool` string semantics consistency; bool↔numeric/string emission fixes; C string escaping fixes; `const` alias conversion-call initializers; bool-ternary comparison normalization; broader conversion runtime/negative coverage.
  - **Step 3 (numeric semantics, completed):** compile-time literal div/mod-by-zero rejection retained; runtime div/mod-by-zero throws `DivisionError`/`DivisionError: Modulo by zero` with traceback in covered contexts/forms (including `let`/`set`, `write`, conditions, returns, call arguments, and for-loop step division for simple/common divisor forms) via runtime helpers/macros, parser-side divisor checks, and codegen guards/rewrites; integer parsing/reads updated to 64-bit (`atoll`/`long long`) in covered paths; overflow behavior defined/characterized (C `long long`/`double`, no explicit overflow trapping).
  - **Step 4 (distinct `int`/`float` semantics, completed):** preserved typed arithmetic for locked/static numeric operands while keeping runtime dynamic `+`/concat for untyped/dynamic values; improved simple-return typing/inference for locked locals and coerced annotated `-> int`/`-> float` returns; `typeof` supports general expressions including function-call expressions in larger expressions; fixed object property-access type inference/result typing; float modulo uses `fmod` with `-lm`; improved assignments/compound ops, comparisons/promotion, unary/inc-dec, array `.length`, interpolation/formatting; added array element type tracking/propagation and typed homogeneous `int`/`float`/`bool` element access in covered expression contexts (arithmetic/comparisons/conditions/typeof/assignments, nested access), numeric-vs-quoted-numeric-string comparison normalization, bare `write(arr[i])` raw/OOB string preservation, and for-each typed/locked iteration over inferred element types. Added/extended `development/tests/runtime/phase1_step4_*` coverage (incl. `phase1_step4_array_elem.*`, `phase1_value_unlocked.*`). **Semantic update (this session, verified):** value-initialized `let` declarations are dtype-unlocked by default (dtype-mutable via `set`) across declaration forms; dtype-only declarations remain locked by default. Parameter symbols are tracked (`is_param`) to preserve dynamic/untyped parameter behavior while allowing typed inference/returns for known-typed unlocked value-initialized locals. **Array assignment typing/complexity audit:** element assignment (`set a[i]=...`) is O(1) bounds-checked replacement with element-type tracking updates; array copy/slice/literal reassignment are O(n) in elements (per-element free/dup/copy), insert/remove are O(n) shifting, push amortized O(1), sort O(n log n). Completed this step: default-typed parameter propagation in bodies/expressions (without locking params), pass-through call-site specialization (`id(5) -> int`, `id(3.5) -> float`, `id("hi") -> string`, nested/chained and first-class/indirect calls), full call-result typing in direct/method/sole-call/argument/`typeof` paths, string-concat wrapper unwrapping for string defaults, array element assignment dtype preservation/coercion and compound element assignment, typed array elements in returns/interpolation/OOB contexts, live-length negative indexing, deep (>3 indices) stack-safety, and `typeof` arity checks. Added `phase1_step4_call_typing.*`, `phase1_step4_default_params.*`, extended `phase1_step4_array_elem.*`, plus a parser negative test for invalid `typeof` array arity. Multi-arch/universal and dist binaries resynced for the completed Step 4 state.

### Key constraints (must respect)
- Preserve existing syntax/features and backward compatibility wherever possible; document intentional behavior changes.
- Compiler pipeline remains integrated (parse + semantic checks in parser in many paths); no unnecessary architecture rewrite.
- Runtime values are largely string-buffer based in generated C (`char[256]` patterns, helper-based operations) — full native type semantics must be implemented within/around this model pragmatically without destabilizing existing behavior.
- `bolt` is the user-facing interface; compiler requires `LTNG_BOLT`/bolt invocation rules as established.
- Every fix must include regression coverage and be validated with native executable execution.

## 3. Implementation steps

> Steps are ordered to reduce risk and enable incremental verification. Some sub-tasks can overlap, but each step should reach a stable, tested checkpoint before moving on.

### Step 1 — Type-model audit and semantics definition (foundation) ✅/baseline
**Goal:** Establish a precise, code-verified understanding of current type representation and define target semantics for Phase 1.

**Tasks**
- Inventory type representations in `ltng.h`, `parser.c`, `codegen.c`, runtime helpers, and symbol table/type-tracking fields.
- Document current behavior vs required behavior for: literals, variables, expressions, assignments, parameters/args, returns, conditions, arrays/objects/null interactions, `typeof`, conversions.
- Define (and document) intended semantics for:
  - `int` vs `float` storage/operations/formatting/comparison
  - numeric promotion and mixed-type operations
  - division/modulo (int/int, float involvement, by zero: compile-time and runtime)
  - overflow behavior (practical policy consistent with runtime model/C backend; document explicitly)
  - `null` compatibility and equality/assignment rules
  - conversion validity/failure behavior (compile-time where provable, runtime otherwise)
- Produce a gap matrix to drive Steps 2–8.

**Deliverables:** audit notes (can live in `PHASE_STATUS.md`/plan updates), clarified semantics decisions, prioritized fix list.
**Exit criteria:** semantics decisions written down; no implementation ambiguity blocking next steps.

**Status:** largely done implicitly via prior audit/fixes; keep decisions explicit as work proceeds.

---

### Step 2 — Explicit conversions and aliases hardening ✅ (stabilized, keep verifying)
**Goal:** Make `toint`, `tofloat`, `tostring`, `tobool`, `tochar` and aliases (`int/float/string/bool/char`) correct and consistent in all supported contexts.

**Tasks**
- Ensure recognition, typing, and emission are correct in: declarations/initializers (`let`/`const`/typed forms as supported), `set`, function args/defaults/named args, returns, conditions/loops/switch expressions, comparisons, arithmetic/mixed expressions, array literals/indexing/ops, `typeof`, interpolation/formatting, nested/complex expressions, and call-result contexts.
- Normalize conversion semantics:
  - `tobool`: consistent truthiness/string parsing (`"t"`,`"f"`,`"true"`,`"false"`,`"0"`,`""`, numeric forms) across compile-time and runtime paths.
  - `tostring`: canonical string forms (incl. bool `"true"/"false"`, float formatting consistency).
  - `toint`/`tofloat`: valid/invalid parsing, whitespace/sign/decimal handling, compile-time rejection for provably invalid literals, defined runtime-invalid behavior.
  - `tochar`: length/range/value rules (define and enforce).
- Fix escaping/formatting/codegen correctness for converted values and string-producing expressions.

**Tests:** positive, negative, boundary, and interaction tests for every conversion in multiple contexts (including nesting and runtime-variable inputs).

**Status:** broadly stabilized; continue adding permutation/edge coverage as gaps are found.

---

### Step 3 — Numeric semantics: division/modulo and overflow ✅ (completed)
**Goal:** Define and implement safe, consistent division/modulo and overflow behavior.

**Tasks**
- **Division/modulo by zero**
  - Compile-time: reject provable literal/constant-zero cases (int and float) with clear diagnostics. ✅
  - Runtime: detect and report runtime-variable/computed-zero cases consistently, throwing `DivisionError: Division by zero` / `DivisionError: Modulo by zero` with traceback (line/col at operator/divisor site where available). ✅ Implemented via generated-runtime helpers/macros (`_ltng_div0_*`, `_LTNG_DIV_*`, `_LTNG_NZDIV_*`), parser-side divisor checks for simple numeric operands (identifiers, int/float literals) in collected expressions, and codegen expression rewrites/guards in covered emission paths (`let`/`set` numeric expressions, `write(...)` numeric expressions, and for-loop step division).
  - Context coverage: assignments, `write`, conditions, returns, call arguments, and loop step expressions covered for common/simple divisor forms; broader/nested/complex divisor subexpressions improved but not exhaustively proven for every possible emitted form.
- **Division semantics**
  - `int / int` truncates toward zero (C/long long integer division semantics). ✅
  - Mixed `int`/`float` division produces `float`. ✅
  - `%` (modulo) supported for integer numeric paths; float modulo uses `fmod` in guarded float-mod paths where applicable. ✅
- **Overflow behavior**
  - **Defined policy:** integer arithmetic is performed using C integer arithmetic in emitted expressions (with `long long`/`atoll` for integer variable reads and 64-bit integer expression paths where emitted) and stored/formatted through the runtime string-buffer model. No explicit runtime overflow trapping is implemented. Signed integer overflow follows underlying C/platform behavior for the emitted operations (commonly two's-complement wraparound for 64-bit `long long` in practice, e.g. `9223372036854775807 + 1 -> -9223372036854775808`), and is documented as implementation-defined/characterized behavior rather than a guaranteed portable numeric result. Float arithmetic uses C `double` semantics. ✅
  - Integer input/read and numeric parsing for integer variable values use 64-bit-capable parsing (`atoll`/`long long`) in updated paths. ✅

**Tests:** compile-time literal div/mod-by-zero negatives; runtime div/mod-by-zero in assignment, `write`, condition, return, and call-argument contexts; for-step safety; mixed-type division/modulo behavior; overflow characterization (`phase1_overflow_semantics`). Regression for existing numeric behavior verified in targeted checks.

**Status:** complete (with documented coverage/policy notes above). Further hardening for rare complex divisor-expression forms can be handled under ongoing Phase 1/type-system stabilization if discovered.

---

### Step 4 — Distinct `int`/`float` semantics across all paths
**Goal:** Make `int` and `float` first-class, consistently distinguished end-to-end.

**Tasks**
- Audit and fix type determination/storage in: lexer literal handling assumptions, `detect_type`, symbol table/type fields, variable declarations/assignments, expression typing (`collect_expression`/equivalent paths), conditions/comparisons, function params/args/returns, array elements/ops, object fields, `typeof`, interpolation/printing/formatting.
- Ensure correct C emission and runtime helper usage per type (int formatting/parsing vs float formatting/parsing, `atoi/atoll/atof/strtod` usage, comparisons, arithmetic).
- Remove unintended int↔float collapsing/merging while preserving intentional compatibility and documented behavior.
- Validate `char`/`bool`/`string`/`null`/`array`/`object` typing remains correct and distinct where required.

**Tests:** `typeof` correctness, arithmetic/comparison semantics per type, storage/round-trip formatting, assignment/update behavior, mixed declarations, regression suite.

**Status:** completed (all listed Step 4 paths verified in the full native suite). Landed so far: typed numeric arithmetic for locked/static numeric operands with runtime dynamic `+`/concat retained for untyped/dynamic values; simple `return <var>` typing/inference for locked `int`/`float`/`bool` locals; annotated `-> int`/`-> float` return value coercion; `typeof` handles general expression arguments and function calls within larger expressions; object property access uses concrete field types in expression typing/inference; float modulo uses `fmod` semantics correctly with `-lm` linking; typed compound assignments/`set` updates, mixed int/float arithmetic/comparisons/promotion, unary/`++`/`--`, array `.length`, and distinct interpolation/printing/formatting in covered paths; array element type tracking infrastructure with propagation through literals/assignments/copies/slices/push/insert/element updates. Latest increments: known homogeneous `int`/`float`/`bool` array element accesses emit typed values in general expression contexts (arithmetic/comparisons/conditions/typeof/assignments, nested access in covered cases), comparison normalization handles numeric expressions vs quoted numeric string literals, bare `write(arr[i])`/multidim element writes preserve raw/OOB string output compatibility, and for-each loop variables use/lock inferred element types for typed iteration/accumulation. Added/extended `development/tests/runtime/phase1_step4_int_float.*`, `phase1_step4_float_mod.*`, `phase1_step4_mod_zero_float.*`, `phase1_step4_paths.*`, `phase1_step4_array_elem.*`, `phase1_value_unlocked.*`. **This session (verified):** value-initialized `let` declarations are dtype-unlocked by default (allowing `set` dtype changes) across declaration forms while dtype-only declarations remain locked; parameter symbols tracked to keep untyped/dynamic parameter behavior while enabling typed inference/returns for known-typed unlocked value-initialized locals. Array assignment typing/complexity audit recorded (element set O(1), copy/slice/literal reassignment O(n), insert/remove O(n), push amortized O(1), sort O(n log n); element-type tracking updated/propagated in covered assignment paths). Final Step 4 increment (verified): default-typed params drive body/expression typing without locking params or breaking `set`; pass-through returns get call-site specialization (including nested/chained and first-class/indirect calls); direct/method/sole-call/argument/`typeof` paths use call-result typing; string concat unwraps numeric wrappers to preserve string defaults; array element assignment preserves homogeneous dtype (coercing the RHS), bool element assignment emits valid strings, compound element assignment (`+=`, `-=`, `*=`, `/=`, `%=`) is supported, negative indexing uses live length after push/pop, OOB/string compatibility is characterized across typed/string/condition contexts, deep non-bare access is stack-safe, interpolation `typeof` handles array elements/calls, and `typeof` reports dimension/arity errors for invalid array indexing. Added/extended `phase1_step4_call_typing.*`, `phase1_step4_default_params.*`, `phase1_step4_array_elem.*`, and `tests/parser/phase1_step4_array_typeof_neg.ltng`. Latest full native suite: **PASS=177 FAIL=0 SKIP=0**. Multi-arch/universal and dist binaries resynced for the completed Step 4 state.

---

### Step 5 — Type inference and numeric promotion
**Goal:** Implement full, consistent inference and promotion rules.

**Tasks**
- Define and implement inference for: literals, variables, unary/binary ops, comparisons/logical ops, calls (including sole-call and indirect/first-class call results where available), conversions, array/object access results, ternary/conditional expressions, and complex/nested expressions.
- Implement numeric promotion rules consistently (e.g. `int op float -> float` where applicable; comparison/coercion rules) across all operators and contexts.
- Handle unknown/dynamic cases conservatively with documented behavior (avoid incorrect typing that causes invalid C or silent misbehavior).
- Ensure promotion/inference interacts correctly with conversions and return typing.

**Tests:** mixed expressions, chained/nested expressions, promotion matrices, inference in declarations/assignments/returns/args/conditions, edge numeric values.

**Status:** partially done (key mixed arithmetic/call cases improved).

---

### Step 6 — Type compatibility checking and assignment/call enforcement
**Goal:** Enforce comprehensive, consistent compatibility rules.

**Tasks**
- Define compatibility matrix for assignments/`set`, parameter binding, argument passing (positional/named/defaults/variadics as applicable), array element operations, return values, and operations requiring specific types.
- Enforce in all contexts with clear compile-time diagnostics before C compilation (reject faulty/unsupported implicit conversions except explicitly allowed ones).
- Reconcile with existing lock/dtype behavior, `null` behavior, `char <-> string` and numeric compatibility decisions; preserve backward compatibility where intended.
- Audit string-concat vs numeric-add heuristics and ensure type-driven behavior is predictable.

**Tests:** invalid/valid assignments, mixed-type operations, null interactions, function arg/param mismatches, array/object type cases, negative compile-time checks.

**Status:** partially done.

---

### Step 7 — Return-type validation
**Goal:** Complete and enforce return-type correctness.

**Tasks**
- Validate consistency of all return paths in a function (including conditionals/loops/try/catch/finally contexts as applicable): consistent types, compatibility with declared/annotated return type (`-> type`) where present, and correct behavior for implicit/unknown returns.
- Ensure missing returns, `void`/value mismatches, and conflicting return types are diagnosed clearly at compile time where possible.
- Align return inference with call-site typing and expression typing (including conversions, anonymous/indirect calls, and propagated values under current runtime model).
- Maintain compatibility with existing pass-through/string-buffer runtime behavior where required.

**Tests:** single/multi-return functions, branch coverage, annotated returns, void/value mismatches, inferred-return call sites, recursion/indirect returns, negative cases.

**Status:** partially done (improvements and annotations in covered cases; comprehensive validation incomplete).

---

### Step 8 — Comprehensive test coverage, hardening, and regression closure
**Goal:** Satisfy Phase 1 validation requirements and stabilize.

**Tasks**
- Expand **positive** tests: all types, conversions in all contexts, inference/promotion matrices, valid mixed expressions, distinct int/float behavior.
- Expand **negative** tests: invalid conversions, incompatible assignments/args/returns, runtime div/mod-by-zero cases, unsupported operations/type mismatches (ensure rejection before C compilation where required).
- Add **boundary/precision** tests: integer/float limits relevant to chosen semantics, floating precision/formatting edge cases, near-overflow behavior, empty/edge string-char cases.
- Add **interaction** tests: conversions + promotion + comparisons + control flow + functions/returns + arrays/objects/null in combinations.
- **Null behavior** matrix tests across operations, assignments, conversions, comparisons, and formatting.
- Run full native suite (`bash development/tests/run_tests.sh`), fix root causes of any failures/regressions, and add permanent regression tests for discovered bugs.
- Memory checks where available (sanitizers/valgrind) for compiler/runtime behavior touched by Phase 1; record results and environment limitations.
- Rebuild/sync multi-arch and universal/dist compiler artifacts when source changes stabilize; verify hashes/dist consistency as applicable.

**Tests:** all of the above; full regression/native executable validation.

**Exit criteria:** Phase 1 completion requirements met; `PHASE_STATUS.md`, `TESTING.md`, `CHANGELOG.md`, `limitations.md` updated with verified results.

**Status:** ongoing.

## 4. Suggested execution order (next slices)

1. **Step 5:** extend full type inference and numeric promotion beyond the Step 4 int/float paths (fix by context/path with targeted tests).
2. **Step 6 + Step 7:** close compatibility and return-validation gaps with comprehensive diagnostics.
3. **Step 8:** exhaustive Phase 1 test matrix, memory checks (where available), full-suite green, artifact rebuild/sync, documentation finalization.

## 5. Risk notes

- **Runtime-model constraints:** string-buffer/rotating-buffer representation limits “true” native semantics; changes must be carefully scoped and compatibility-tested.
- **Expression/codegen fragility:** complex string-based expression collection/emission can regress easily; prefer targeted, well-tested fixes and avoid broad unsafe rewrites.
- **Behavioral changes:** stricter typing/checking or corrected semantics may break legacy reliance on heuristic behavior; document and minimize intentional breakage.
- **Performance/code size:** runtime safety checks/helpers may affect generated C; keep implementations simple and correct first.

## 6. Tracking checklist

- [x] Step 1: audit/semantics baseline (initial)
- [x] Step 2: conversion/alias stabilization (broad; ongoing verification)
- [x] Step 3: division/modulo and overflow semantics (compile-time + runtime div/mod-by-zero handling, for-step safety, 64-bit integer parsing/read paths, defined/characterized overflow policy and tests)
- [x] Step 4: distinct `int`/`float` semantics across all paths
- [ ] Step 5: full type inference and numeric promotion
- [ ] Step 6: comprehensive type compatibility checking
- [ ] Step 7: complete return-type validation
- [ ] Step 8: full Phase 1 test/validation/memory/doc closure and artifact sync

_Update this file and `development/PHASE_STATUS.md` at each verified checkpoint._
