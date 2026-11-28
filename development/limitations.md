# Lightning Limitations (Phases 0–4)

This document consolidates known limitations, gaps, and unresolved issues recorded so far for Phases 0–4 of the Lightning enhancement effort. It reflects the current repository state, implementation status, test coverage, and documented constraints.

> Status context: Phase 0 complete, Phase 1 complete (verified; env-limited memory/artifact caveats recorded), Phase 2 complete, Phase 3 complete (with key limitations reduced), Phase 4 complete. Phases 5–13 are pending/in progress.

---

## Baseline / cross-cutting limitations (pre-existing or still relevant)

These limitations were identified in the initial audit and remain relevant across phases unless explicitly addressed.

### Language/runtime architecture and representation
- **String-buffer runtime model (largely removed):** core runtime values are stored as dynamic heap strings (`_ltng_ds_*`), and generated formatting/temp/return-buffer helpers use dynamic allocation (`_ltng_ds_fmt`, `_ltng_retbuf_fmt`, dynamic `_ltng_retbuf`). Exception payload/trace/function-stack runtime buffers are also dynamic. Fixed-size `char ...[N]` scratch buffers have been removed from the compiler source files; parser/codegen scratch space is heap-allocated as needed.
- **Static/rotating runtime buffers:** rotating return/temp buffers remain a lifetime/aliasing constraint (`_ltng_retbuf` and similar patterns), but they are heap-backed/dynamic rather than fixed-size static storage.
- **Fixed limits in compiler/runtime paths (largely addressed):** legacy fixed-limit macros (`MAX_STRING_LEN`, `MAX_IDENTIFIER_LEN`, `MAX_TOKENS`, `MAX_IDX_W`, `CG_MAX_CLEANUPS`, `CG_MAX_LOOPS`, etc.) are removed from compiler source, and `strncpy/strncat/sprintf`/fixed `char[N]` source buffers have been eliminated. Key APIs such as `parse_expression_from_string`/collect-expression paths use growable heap buffers. Remaining numeric sizes are mostly initial/growable heap capacities or IO chunk sizes; a few caller-bounded API writes and generated runtime C patterns remain and are tracked in `development/STATIC_LIMITS_AUDIT.md`.
- **Heuristic typing/coercion behavior:** dynamic/numeric-string heuristics (e.g. numeric-string detection/add behavior) can produce surprising results and are not yet fully aligned with a robust general-purpose type system.
- **`+` operator overload ambiguity (add vs concat):** `+` is overloaded for numeric addition and string concatenation, but dispatch is context/type-dependent and can be inconsistent or order-sensitive in mixed string/numeric cases. Examples (current behavior): `"5" + 5` and `5 + "5"` both evaluate to `10` (not `"55"`), while with typed variables `let s = "5"; let n = 5` yields `s + n == "55"` (string concat) but `n + s == 10` (runtime numeric/string add path). Deterministic intent currently requires explicit conversion/careful typing rather than relying on mixed-operand `+` semantics.

### Compiler implementation
- **Integrated parse/semantic pipeline:** semantic analysis and type tracking are largely integrated into parsing/expression collection rather than separated into fully independent, unit-testable stages.
- **Complex expression/codegen paths:** string-oriented expression collection and specialized code generation paths are fragile for new constructs and require careful regression testing.
- **Build warnings (addressed):** compiler source builds cleanly with default flags (`-Wall -Wextra -pedantic -std=c99`) and also with `-Wformat-truncation=2` (as of latest verified build). Continue keeping new code warning-clean.
- **Generated C safety/visibility:** normal compile paths suppress backend C warnings in some workflows; generated code and helper usage still need ongoing safety/overflow/truncation auditing.

### Memory and ownership
- **No comprehensive ownership strategy:** runtime arrays/strings/objects/closures do not yet share a complete, unified ownership/lifetime model.
- **Compiler memory not fully audited:** dynamic allocations in compiler internals are not comprehensively audited/freed across all paths.
- **Runtime cleanup not globally proven:** while specific Phase 3 array mutation/copy/slice cleanup paths were improved, generated programs do not implement a full automatic cleanup/GC model, and memory correctness is not comprehensively proven.

### Diagnostics and tooling
- **Diagnostics are basic:** file/line/column/source/caret errors exist, but richer diagnostics (error codes taxonomy, warnings, suggestions quality, runtime stack traces) are limited.
- **Tooling availability varies:** sanitizer/fuzz/leak-detection tooling is environment-dependent; results must be recorded when available.

### Compatibility and behavior risk
- **Ongoing semantic evolution risk:** improvements to typing, scoping, arrays, and runtime behavior may conflict with legacy heuristic behavior; compatibility must be preserved where possible and intentional changes documented.

---

## Phase 0 — Testing infrastructure

**Status:** Complete (baseline infrastructure established)

### Limitations / gaps
- **Compiler stages not separately unit-testable as libraries:** lexer/parser/semantic/codegen stages are validated primarily through CLI behavior (`--check`, `--emit-c`, compile+run native executables), not isolated internal APIs.
- **Sanitizer/fuzz tooling not consistently available:** availability varies by environment; sanitizer/fuzz/memory-leak automation is not uniformly recorded or enforced by the baseline harness.
- **Baseline coverage breadth is limited:** infrastructure and initial smokes/regression/negative coverage exist, but category-specific suites (lexer/parser/semantic/runtime/codegen/e2e/native/adversarial) require ongoing expansion.
- **Negative classification edge cases:** some corpus files require special handling (e.g. `test_errors2.ltng` is comment-only/valid and excluded from negative classification).
- **Environment/workspace execution constraints:** compiler binaries may require copying to an executable temp location (e.g. `/tmp/opencode/ltng`) in some environments.
- **Build warnings addressed:** default and `-Wformat-truncation=2` builds are clean as of latest verified build; keep warning-clean discipline for new code.

---

## Phase 1 — Type system

**Status:** Implemented and verified for current Phase 1 scope (see PHASE_STATUS.md; some hardening/portability caveats remain)**

### Implemented so far (relevant context)
- Distinct `float` detection/reporting and type-name improvements in key paths; improved float expression/type tracking and float expression output in covered cases.
- Expanded conversion support/handling for `toint`/`tofloat`/`tostring`/`tobool`/`tochar` in more contexts (expressions, comparisons, interpolation/`typeof` mapping, argument/return paths covered by tests), with stricter compile-time rejection of obvious invalid literal numeric strings for `toint`/`tofloat` in validated contexts.
- Compile-time detection/rejection of literal division/modulo by zero (int and float literal forms).
- Improved numeric promotion in key mixed expressions, comparison normalization for numeric conversion helpers, and return/call/indirect-call expression typing fixes (including self-reference/body-parsing cases) to prevent incorrect string typing/internal compile failures.
- Support for explicit type declarations `let x = float` and `let x = char`.
- Additional Phase 1 runtime/semantic negative tests added, including Step 2 conversion/alias coverage and follow-up stabilization fixes; latest full suite passes (**234 PASS, 0 FAIL, 0 SKIP**). Step 2 conversion behavior is broadly exercised but not exhaustively proven for every permutation/context.

### Remaining limitations
- **Distinct int/float semantics (addressed in covered paths):** Step 4/5 work made int/float behavior consistent across covered expression, assignment, comparison, codegen, and runtime paths; rare edge cases may remain in less-common emitted forms.
- **Type inference (addressed in covered paths):** Step 5 completed inference/promotion coverage for covered constructs/contexts; dynamic/untyped cases remain conservatively typed.
- **Conversion coverage improved but not exhaustive:** `toint`/`tofloat`/`tostring`/`tobool`/`tochar` and aliases are supported and tested in many contexts. Runtime-invalid numeric strings for `toint`/`tofloat` now throw `ValueError` in covered expression-conversion paths (compile-time invalid literals already rejected; runtime variable/complex string conversions validated in conditions, array elements, returns/calls, and general expressions). Exhaustive coverage/verification across every permutation and edge context is not complete.
- **Numeric promotion rules (addressed in covered paths):** Step 5 implemented promotion in covered operators/contexts; continue auditing uncommon emitted-expression forms.
- **Type compatibility checking (conservatively enforced):** Step 6 added compile-time rejection for provably incompatible locked/dtype-fixed assignments, string/char literal operands in `/`/`%`, unary numeric operators on string/char literals, and provably non-numeric array indices in covered expression/write/set contexts. Dynamic/untyped pass-through behavior is preserved; not every possible type mismatch is provably detectable at compile time.
- **Return-type validation (enforced in covered/provable cases):** Step 7 enforces annotated return mismatches for provable literal/locked-variable cases, void-function value returns, missing returns on some control paths for annotated non-void functions, and sound `NODE_TRY` always-exits handling. Dynamic/untyped pass-through returns remain allowed where not provably incompatible.
- **Numeric semantics gaps (Step 3 completed for defined scope, with remaining hardening):** compile-time literal division/modulo by zero is rejected; runtime variable/computed division/modulo by zero throws `DivisionError` / `DivisionError: Modulo by zero` in covered contexts/forms (assignments/`let`/`set` including scalar compound assignments, `write`, conditions, returns, call arguments, for-loop step division, and complex divisor subexpressions such as `(b-c)`, `(d+0)`, `(d*5)`) using parser-side checks, runtime helpers/macros, and codegen guards/rewrites. Coverage is broad but not exhaustively proven for every unusual emitted-expression form.
- **Overflow behavior (defined/characterized, not trapped):** integer arithmetic uses emitted C integer operations with 64-bit parsing (`atoll`/`long long`) in updated paths and no explicit runtime overflow trapping; signed overflow follows underlying C/platform behavior (characterized in tests, e.g. 64-bit wraparound in practice). Float arithmetic uses C `double`. Precision/boundary handling and portable overflow guarantees remain limited by the runtime/codegen model.
- **Test coverage/memory checks (improved, environment-limited):** Phase 1 positive/negative/boundary/interaction coverage expanded (including Step 6/7 negatives and existing Phase 1 runtime/semantic suites); full native suite is green. UBSAN full-suite is clean in the validated environment; ASan is unusable in this proot/container environment and valgrind unavailable, so memory-safety evidence is not comprehensive.
- **Runtime-model constraints:** underlying string-buffer/runtime representation limits how fully native numeric/type semantics can be realized without broader redesign.

### Intentional/documented behavior changes (compatibility impact)
- `typeof(...)` reports `float` for covered float cases (instead of previous merged `int` behavior), which can affect programs/tests relying on older reporting.
- Some previously-accepted faulty inputs are now rejected at compile time before C compilation (e.g. literal `x / 0`, `x % 0`, and obvious invalid literal numeric strings passed to `toint`/`tofloat` in validated contexts).

---

## Phase 2 — Semantic analysis and scopes

**Status:** Complete (implemented and regression-tested; bare blocks now supported)

### Implemented (additional)
- **Bare blocks:** standalone `{ ... }` blocks are supported as scoped blocks (with local declarations/shadowing consistent with existing block-scope semantics).

### Limitations / notes
- **Scope/lifetime semantics tied to current runtime model:** block-scoped declarations and lifetimes are implemented within the existing string-buffer/runtime representation; further hardening may be required as later phases change runtime representations.
- **No guarantee of full semantic pass separation:** semantic checks remain closely integrated with parsing/code generation architecture.
- **Advanced scoping features not addressed in this phase:** closures/capture semantics and related lifetime/ownership concerns are outside Phase 2 scope and remain pending for later phases.

---

## Phase 3 — Arrays and collections

**Status:** Complete (implemented and regression-tested; key limitations reduced)

### Implemented (relevant context)
- Dynamic array storage (`char **data`, `len`, `cap`) with runtime helpers and growth/reallocation.
- Core APIs: indexing get/set, `length`, `push`, `pop`/`pop(index)`, `insert`, `remove`, `contains`, `index_of`, `sort`, `reverse`.
- Copy/slice/assignment in `let` and `set` forms: `let/set b = a`, `let/set b = a.copy()`, `let/set b = a.slice(start,end)`.
- For-each iteration: `for (x in arr) { ... }` (plus index-based iteration).
- Indexing semantics preserved: 1-based positive, negative-from-end, index `0` invalid, out-of-bounds/invalid reads return empty string.
- Bug fixes applied: missing `_ltng_arr_set` helper for element assignment; `_ltng_arr_copy`/`_ltng_arr_slice` overwrite cleanup fixes.
- Numeric-aware `sort` for fully numeric element pairs (otherwise string comparison).

### Remaining limitations

#### Collection/value model
- **Arrays of objects not supported as first-class dynamic array collections:** object values/field semantics are not represented as array element collections in the Phase 3 runtime model.
- **Jagged/nested dynamic arrays (arrays-of-arrays as values) not supported:** general nested/variable-shape array collections are not supported; **rectangular multidimensional arrays remain supported**.
- **Arrays are auto-locked (dtype-immutable) after declaration:** preserved behavior; dynamic dtype/homogeneity evolution is not provided by Phase 3.

#### API and expression-context limitations
- **`copy()` / `slice()` in general expression contexts (partial):** `copy()` and `slice()` work in `let`/`set` array assignment forms and also in general/value-consumed expressions on temp arrays (including chaining and returning scalar or formatted string results). However, they do not produce first-class array values that can be directly assigned as arrays outside supported `let`/`set` forms.
- **Arrays are not fully first-class expression values (reduced):** function-returned arrays are supported via runtime array-handle packing/unpacking, enabling `let/set a = getArray()`, `return arr`, `return getArray()`, and chaining on array-returning calls (e.g. `getArray().sort()`, `getArray().copy()`) in value-consumed contexts. General array-typed function parameters are supported when the function body uses array operations/indexing/`for (x in arr)` on the parameter (parser pre-scan), using ah handle pass/copy semantics. General array-producing temporary values remain limited by the non-first-class array model.
- **Base-parameter UDMs on array receivers (partial):** supported for statement/value-consumed calls when the function body uses array operations/indexing/`for (x in base)` on `base` (detected by parser pre-scan), using ah handle passing/writeback. Functions whose `base` param is not detected as array-used still treat `base` as a string value.

#### Semantics tied to current runtime/type model
- **String-buffer element representation:** array elements are stored/formatted as string-backed runtime values.
- **Comparison/search semantics follow formatted values:** `contains` and `index_of` compare formatted string values.
- **Sorting semantics improved but not fully typed:** `sort` is numeric-aware only when both compared elements are fully numeric; otherwise string comparison is used. Full typed/value-consistent sorting and operation semantics remain constrained by the runtime/type system.
- **Homogeneity/mutation semantics are runtime-model driven:** no separate strict, fully general typed array-element system beyond current language/runtime behavior.

#### Aliasing, ownership, and memory
- **No true shared-reference aliasing model for arrays:** assignment/copy/slice operations use copy semantics for safety in the current ownership model; shared mutable backing storage with robust multi-owner semantics is not implemented.
- **No comprehensive automatic cleanup/GC:** runtime helpers allocate/free/replace elements on mutation/copy/slice/pop/remove paths, but generated programs do not provide a complete ownership/lifetime management framework for all values/scopes.
- **Memory validation not fully proven:** UBSAN full-suite is clean in the validated environment; ASan is unusable in this proot/container environment and valgrind unavailable. Large-array stress/performance characterization not recorded.

#### Behavior and edge-case notes (preserved/defined)
- **Index `0` is invalid** for array access/set semantics (returns empty/no-op behavior as implemented).
- **Out-of-bounds/invalid indexed reads return empty string**; invalid indexed mutations/removals are bounds-guarded/no-op where applicable.
- **1-based positive and negative-from-end indexing** is preserved by design.

---

## Phase 4 — Exception handling

**Status:** Complete (implemented and regression-tested)

### Implemented (relevant context)
- `try`/`catch`/`finally`/`throw`, optional catch variable, custom error types, `error(type, message)`, and bare rethrow inside `catch`.
- Catch object fields: `type`, `message`, `trace`, `line`, `column`, `function`, `value`, `obj`.
- Propagation through nested `try` blocks and function calls; uncaught exceptions print to stderr and exit with status `1`.
- Call-site-aware stack traces using hidden call-site line/column arguments.
- `finally` runs on normal completion, `throw`, `return`, `break`, and `continue`; `return` in `finally` overrides pending control flow; new exceptions in `finally` replace pending ones.

### Remaining limitations
- **Function stack depth limited by memory/C stack:** the runtime function-name/trace stack is heap-backed and grows dynamically, but extremely deep recursion can still exhaust process stack/memory.
- **Dynamic exception payloads/traces:** exception type, message, value, trace, and function-stack storage are heap-backed/dynamic (not fixed-size buffers), so very large payloads are limited by available memory rather than fixed buffer sizes.
- **Typed catch filtering:** `catch (Type e)` is supported for exact `e.type` string match. Multiple typed catch clauses on the same try are supported; the first matching catch runs, non-matching catches are skipped, and if none match the exception is rethrown (finally blocks still run). Sibling catches do not catch exceptions thrown by another sibling catch body. Catch-all clauses (`catch (e)` / `catch e`) make subsequent catch clauses unreachable.
- **`exit` unwinds `finally`:** `exit` now evaluates its exit code, unwinds pending `finally` blocks, then exits. A `return`/`throw` inside `finally` can override the pending exit (consistent with existing finally override semantics).
- **Inlined cleanup / code size:** `finally` bodies are inlined at each exit point (including exception unwinding), which can increase generated code size for deeply nested `try`/`finally` blocks.
- **Saved-exception stack depth:** pending exceptions are preserved in stack-local `LtngExcSaved` values during exception-path `finally` execution; values are heap-backed/dynamic, but very deep exception-path `finally` nesting still consumes additional C stack.
- **Exception object model:** catch values are object-like runtime values; they are not first-class exception instances with inheritance, custom fields, or user-defined exception classes.
- **Memory validation:** UBSAN full-suite runs are clean in the validated environment; ASan was unusable in this proot/container environment and valgrind unavailable, so memory-safety evidence is not comprehensive.

### Behavior notes (defined)
- A bare `throw` outside a `catch` block is a compile-time error.
- `try` requires at least one of `catch` or `finally`.
- `throw true` / `throw false` produce message `"true"` / `"false"`.
- A `return` in `finally` overrides a pending `return` or exception; a new exception thrown in `finally` replaces a pending exception.
- `break` / `continue` in `finally` leave the enclosing loop after cleanup, discarding any pending exception.

---

## Documentation and test-coverage limitations related to Phases 0–4

- **Phase 1 hardening caveats remain:** Phase 1 is verified complete for current scope, but conversion/runtime-invalid edge permutations, provable-check limits, and environment-limited memory-check evidence remain (see Phase 1 section).
- **Phase 0 harness is foundational, not exhaustive:** category-specific and adversarial/fuzz/memory automation suites need continued expansion.
- **Memory/tooling evidence is environment-limited:** where sanitizers/leak tools are unavailable or incompatible, results cannot be claimed beyond executed native tests.
- **Some limitations are architectural:** arrays-of-objects/jagged collections, first-class array expressions, full typed semantics, and robust shared ownership/cleanup are likely to require broader runtime/design changes in later phases.

---

## Summary

Phases 0–4 established reproducible testing infrastructure, improved scope semantics, delivered a substantially more capable dynamic array system, and added structured exception handling with stack traces and `finally` cleanup while preserving existing behavior. However, major limitations remain in **type semantics hardening (Phase 1 caveats)**, **runtime/value representation and ownership**, **first-class collection/expression semantics**, **exception model depth/ergonomics**, and **comprehensive memory/tooling validation**. These are expected to be addressed incrementally in future phases where feasible, with compatibility preserved unless intentionally documented.
- **Anonymous functions/closures/named function references (Phase 5, partial):** `function(params){...}` expressions, closure capture of enclosing-scope variables with **shared, mutable** semantics, named function references as first-class values, returning/passing function values, and dynamic dispatch through variables/parameters are supported. Variadic arguments and function overloading are now implemented (see above); return-type checking is annotation/inference based and richer generic type annotations remain future work.
- **Immediate chained calls:** chained invocation of call results (e.g. `maker()(5)`, multi-chain) is supported in general expression contexts, including sole RHS contexts (`let`/`const`/`set`/`return`), conditions/comparisons, arithmetic expressions, `write(...)` arguments, and standalone call statements. Deep chains are limited by internal expression-buffer/complexity bounds (very long chains may produce a compile-time "Chained call expression too deep/complex" error; in tested cases ~20 chained groups work). Edge cases in highly complex expression forms may still exist.
- **Sole-call typing inference (partial):** type inference for sole-call RHS/assignments relies on inferred function return types; mixed/unknown returns may still be treated conservatively as string-typed in some paths, and complex non-sole expressions continue to follow existing expression typing rules.
