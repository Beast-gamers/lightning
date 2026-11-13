# Lightning Limitations (Phases 0–3)

This document consolidates known limitations, gaps, and unresolved issues recorded so far for Phases 0–3 of the Lightning enhancement effort. It reflects the current repository state, implementation status, test coverage, and documented constraints.

> Status context: Phase 0 complete, Phase 1 partially implemented, Phase 2 complete, Phase 3 complete (with key limitations reduced). Phases 4–13 are pending.

---

## Baseline / cross-cutting limitations (pre-existing or still relevant)

These limitations were identified in the initial audit and remain relevant across phases unless explicitly addressed.

### Language/runtime architecture and representation
- **String-buffer runtime model:** most runtime values are represented using fixed `char[256]` buffers in generated C and supporting runtime helpers; this constrains value size, representation, and ownership semantics.
- **Static/rotating runtime buffers:** helpers such as rotating return/temp buffers (e.g. `_ltng_retbuf` and similar patterns) create lifetime/aliasing constraints and are not a general ownership model.
- **Fixed limits in compiler/runtime paths:** legacy/structural limits still exist in parts of the implementation (e.g. `MAX_STRING_LEN`, identifier/token-related limits, fixed buffer formatting in some paths, dimension/index handling constraints in specific contexts).
- **Heuristic typing/coercion behavior:** dynamic/numeric-string heuristics (e.g. numeric-string detection/add behavior) can produce surprising results and are not yet fully aligned with a robust general-purpose type system.

### Compiler implementation
- **Integrated parse/semantic pipeline:** semantic analysis and type tracking are largely integrated into parsing/expression collection rather than separated into fully independent, unit-testable stages.
- **Complex expression/codegen paths:** string-oriented expression collection and specialized code generation paths are fragile for new constructs and require careful regression testing.
- **Build warnings partially addressed:** several compiler-source warnings were fixed (unused functions/parameters/variables, missing switch/default handling in key paths, label/declaration issues, embedded-NUL format issue, const-correctness in a helper path, and path buffer sizing in `main.c`). However, many format-truncation warnings still remain in complex expression/codegen formatting paths and require careful refactoring to eliminate safely.
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
- **Build warnings not fully eliminated:** some compiler-source warnings were addressed in later fixes, but format-truncation warnings in complex paths remain and are tracked as cross-cutting technical debt.

---

## Phase 1 — Type system

**Status:** Partially implemented (core improvements landed; further work required)

### Implemented so far (relevant context)
- Distinct `float` detection/reporting in key paths; `typeof` improved for covered float cases.
- Improved type name reporting for `float`/`char` in key paths.
- Initial recognition/handling for `tofloat`/`tochar` in some contexts; support for `let x = float` and `let x = char`.
- Numeric compatibility adjustments in key assignment checks (`int <-> float`, `char <-> string` compatibility retained in relevant cases).

### Remaining limitations
- **Incomplete distinct int/float semantics:** not fully consistent across all expression, assignment, comparison, codegen, and runtime paths.
- **Incomplete type inference:** full and consistent inference across all language constructs/contexts is not complete.
- **Explicit conversion coverage incomplete:** `tofloat`/`tochar` (and conversion behavior generally) not fully implemented/verified in all contexts (including interpolation/evaluation and all expression forms).
- **Numeric promotion rules incomplete:** consistent promotion/coercion rules are not fully defined/implemented across all operations.
- **Type compatibility checking incomplete:** comprehensive compatibility and enforcement across all assignment, operation, parameter, and return contexts is not complete.
- **Return-type validation incomplete:** function return type checking/validation is not fully implemented.
- **Numeric semantics gaps:** overflow behavior, division semantics, precision behavior, and boundary handling are not fully specified/verified.
- **Insufficient negative/boundary/interaction coverage:** required comprehensive tests for invalid conversions, mixed expressions, precision/boundary values, null behavior interactions, and memory checks (where available) are not complete.
- **Runtime-model constraints:** underlying string-buffer/runtime representation limits how fully native numeric/type semantics can be realized without broader redesign.

### Intentional/documented behavior changes (compatibility impact)
- `typeof(...)` reports `float` for covered float cases (instead of previous merged `int` behavior), which can affect programs/tests relying on older reporting.

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
- **`copy()` / `slice()` not available in all expression contexts:** supported in `let` and `set` array assignment forms, but not as general array-producing expressions/temporaries in arbitrary expression positions.
- **Arrays are not first-class expression values:** limits chaining, passing/returning array-producing expressions, and general expression-context array operations under the current codegen/runtime model.

#### Semantics tied to current runtime/type model
- **String-buffer element representation:** array elements are stored/formatted as string-backed runtime values.
- **Comparison/search semantics follow formatted values:** `contains` and `index_of` compare formatted string values.
- **Sorting semantics improved but not fully typed:** `sort` is numeric-aware only when both compared elements are fully numeric; otherwise string comparison is used. Full typed/value-consistent sorting and operation semantics remain constrained by the runtime/type system.
- **Homogeneity/mutation semantics are runtime-model driven:** no separate strict, fully general typed array-element system beyond current language/runtime behavior.

#### Aliasing, ownership, and memory
- **No true shared-reference aliasing model for arrays:** assignment/copy/slice operations use copy semantics for safety in the current ownership model; shared mutable backing storage with robust multi-owner semantics is not implemented.
- **No comprehensive automatic cleanup/GC:** runtime helpers allocate/free/replace elements on mutation/copy/slice/pop/remove paths, but generated programs do not provide a complete ownership/lifetime management framework for all values/scopes.
- **Memory validation not fully proven:** no recorded sanitizer/valgrind/leak-stress validation in this environment (ASan was not usable here; valgrind not available). Large-array stress/performance characterization was not recorded.

#### Behavior and edge-case notes (preserved/defined)
- **Index `0` is invalid** for array access/set semantics (returns empty/no-op behavior as implemented).
- **Out-of-bounds/invalid indexed reads return empty string**; invalid indexed mutations/removals are bounds-guarded/no-op where applicable.
- **1-based positive and negative-from-end indexing** is preserved by design.

---

## Documentation and test-coverage limitations related to Phases 0–3

- **Phase 1 requirements not fully met:** remaining type-system work and required comprehensive positive/negative/boundary/interaction/memory tests are incomplete.
- **Phase 0 harness is foundational, not exhaustive:** category-specific and adversarial/fuzz/memory automation suites need continued expansion.
- **Memory/tooling evidence is environment-limited:** where sanitizers/leak tools are unavailable or incompatible, results cannot be claimed beyond executed native tests.
- **Some limitations are architectural:** arrays-of-objects/jagged collections, first-class array expressions, full typed semantics, and robust shared ownership/cleanup are likely to require broader runtime/design changes in later phases.

---

## Summary

Phases 0–3 established reproducible testing infrastructure, improved scope semantics, and delivered a substantially more capable dynamic array system with preserved indexing behavior. However, major limitations remain in **type-system completeness (Phase 1)**, **runtime/value representation and ownership**, **first-class collection/expression semantics**, and **comprehensive memory/tooling validation**. These are expected to be addressed incrementally in future phases where feasible, with compatibility preserved unless intentionally documented.