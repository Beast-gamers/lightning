# Dynamic Limits Plan

This document outlines a practical, incremental plan to remove remaining fixed compiler/runtime limits in Lightning (`ltng`) after the runtime string-truncation fix and partial dynamic-limit hardening.

## Goals

- Eliminate fixed-capacity failures and silent truncation across compiler and generated runtime paths.
- Replace compile-time size assumptions with growable/dynamic storage where feasible.
- Preserve language behavior and keep the full test suite green at each step.
- Prefer incremental refactors with clear ownership/lifetime rules over a single risky rewrite.

## Current status (already dynamic / improved)

- **Generated runtime value storage is dynamic** in covered paths: locals/params/returns/reads, string concat/repeat, formatting, rotating dynamic slots/return buffers, and related emitted helpers (`_ltng_ds_*`, dynamic concat/repeat, dynamic read input).
- **Compiler structures partially dynamic**:
  - `ErrorState` errors/messages/source lines (`source_code/compiler/source/src/errors.c`, `ltng.h`)
  - `Scope` variable storage (`vars`, `vars_cap`) with dynamic growth (`parser.c`, `ltng.h`)
- **Lexer token buffer capacity is dynamic** (`tokens` array grows via realloc in `lexer.c`).
- **Still fixed / size-limited** (primary targets below): token/identifier/AST/value fields, object/function/param/capture/signature capacities, parser/codegen scratch and expression buffers, and generated runtime exception/trace stack/buffer limits.

## Inventory of remaining fixed limits (key targets)

### 1) Core token and symbol/AST string storage (`ltng.h`, widespread usage)

- `MAX_STRING_LEN` / `MAX_IDENTIFIER_LEN` constants still used broadly.
- Fixed struct fields:
  - `Token.value[MAX_STRING_LEN]`
  - `ASTNode.name[MAX_IDENTIFIER_LEN]`, `ASTNode.value[MAX_STRING_LEN]`
  - `ASTNode.variadic_name`, `captures[8][MAX_IDENTIFIER_LEN]`, `return_obj_base`, `for_var`, `for_start/end/step`, `array_index`
  - `VarEntry.name/cname/func_ref_cname`
  - `ObjField.name/value`, `ObjSchema.var/default_val`
  - `FuncBind.src/cname`
- Many parser/codegen sites assume fixed buffers (`strncpy`/`snprintf` with `sizeof(...)`, direct indexing, stack temporaries sized by these macros).

### 2) Fixed collection capacities

- `MAX_OBJ_FIELDS` (`ObjSchema.fields[MAX_OBJ_FIELDS]`, loops/copies capped)
- `MAX_FUNCBINDS` (`Scope.funcs[MAX_FUNCBINDS]`)
- `MAX_VARS` constant remains (scope storage is dynamic, but other paths/constants may still assume fixed behavior)
- `ASTNode.captures[8][MAX_IDENTIFIER_LEN]` fixed capture count and fixed name buffers
- Function parameter/argument/overload/signature and related compiler-side registries/tables (audit for fixed arrays and hard caps in `parser.c`, `codegen.c`, `ir.c`)

### 3) Parser/codegen scratch and formatting buffers

- Large number of stack buffers sized by `MAX_STRING_LEN`/`MAX_IDENTIFIER_LEN`/hardcoded sizes in:
  - `parser.c` (expression collection/building, interpolation, conversion emission, argument formatting, type/name resolution)
  - `codegen.c` (name mangling helpers, array/index/name formatting, runtime helper emission, IR/codegen logging, emitted C snippets)
  - `ir.c` and error/reporting helpers where applicable
- Format-truncation warnings indicate remaining bounded `snprintf` paths for generated names/expressions.

### 4) Generated runtime limits (emitted C)

- Fixed exception/function-trace storage and frame capacity in emitted runtime (`codegen.c` runtime prelude):
  - `LTNG_FN_MAX` and fixed arrays for function stack frames
  - fixed `_ltng_exc_type/msg/value/trace/func` buffers
- Other emitted fixed buffers (e.g. small helper temporaries) should be audited and converted where they can truncate user-visible data.

## Guiding principles

1. **Separate capacity growth from ownership changes**: making arrays grow is easier than changing struct field ownership from inline arrays to heap pointers.
2. **Introduce helpers first**: string/buffer/growable-array helpers reduce duplicated unsafe patterns.
3. **Migrate struct layouts carefully**: changing `Token`/`ASTNode`/`VarEntry` fields to heap-owned pointers requires explicit init/copy/free and clone semantics.
4. **Test after each phase**: run targeted cases plus `bash development/tests/run_tests.sh` frequently.
5. **Avoid semantic changes**: limit refactors should not alter language behavior except removing truncation/capacity failures.

## Phased implementation plan

### Phase 0 — Baseline and safety net (prerequisite for each phase)

- Keep a clean baseline and run full tests before/after each chunk.
- Add/retain targeted stress cases for large inputs (long strings, long identifiers, deep/large expressions, many params/fields/captures, long traces/messages where feasible).
- Capture current warning hotspots (`snprintf` truncation, unused helpers) but prioritize correctness.

**Validation:** `bash development/tests/run_tests.sh` (must remain green).

### Phase 1 — Replace pervasive stack scratch buffers with growable heap buffers

**Objective:** Remove truncation/stack-pressure risks in parser/codegen without changing public struct layouts yet.

**Steps:**
1. Introduce small internal dynamic buffer/string-builder utilities (or reusable growable char buffer helpers) for compiler-side temporary construction in `parser.c`/`codegen.c` (and `ir.c` if needed).
2. Refactor high-risk formatting paths that build large expressions/strings into fixed buffers:
   - Expression collection/assembly, interpolation expansion, conversion/call argument emission strings, concatenated C fragments.
   - Replace repeated `char buf[MAX_STRING_LEN]` / `snprintf(buf, sizeof(buf), ...)` patterns with growable buffers where output length is unbounded.
3. Convert fixed arrays used as temporary argument/name lists where counts can exceed assumptions.
4. Re-run tests after each logical substep.

**Benefits:** Reduces recurrence quickly, lowers stack-overflow risk, prepares for dynamic struct fields.

**Risks:** Lifetime/ownership bugs for temporary strings passed across functions.

### Phase 2 — Make token text storage dynamic (`Token.value`)

**Objective:** Remove fixed-size token value/lexeme storage limit.

**Steps:**
1. Change `Token.value` from fixed array to heap-owned pointer (or length+pointer) in `ltng.h`.
2. Update lexer allocation/initialization/free paths (`lexer.c`): allocate/resize per token text, ensure `lexer_free` releases token strings, define clear ownership semantics.
3. Audit and update token-copy behavior:
   - `lexer_peek/lexer_next` return-by-value creates shallow copies; decide and enforce policy (shared/read-only borrows vs deep-clone helpers where mutation/independent lifetime is needed).
   - Fix temp-lexer/token duplication sites (e.g. default-arg evaluation and any synthetic token construction) to avoid double-free/leaks.
4. Replace all `token.value[...]` fixed-buffer assumptions and unsafe indexing with null-safe access.
5. Rebuild and run targeted lexer/parser cases, then full suite.

**Risks:** Double-free/use-after-free due to aliased token values; regressions in parser code that mutates token buffers.

### Phase 3 — Make AST and symbol/identifier storage dynamic

**Objective:** Remove fixed string capacities in AST/symbol/object/function metadata.

**Steps:**
1. Convert `ASTNode` string fields (`name`, `value`, `for_*`, `array_index`, `variadic_name`, `return_obj_base`, `captures` names, etc.) to dynamic strings/pointers with consistent init/free helpers.
2. Update AST node creation/cloning/freeing (`parser.c` `create_node`/related helpers, `codegen_free` / AST teardown paths) to allocate/copy/release owned strings.
3. Convert `VarEntry`/`ObjField`/`ObjSchema`/`FuncBind` identifier/value fields to dynamic storage; update symbol/object registration, lookup, copying, and emission code.
4. Replace `sizeof(field)`-based formatting and truncating copies with dynamic-safe writes.
5. Run tests after each struct family migration.

**Risks:** Widespread mechanical changes; missing frees/leaks; incorrect copies when nodes/schemas are duplicated.

### Phase 4 — Remove fixed collection/capacity limits

**Objective:** Eliminate hard caps on counts (not just string lengths).

**Steps:**
1. **Object fields**: make `ObjSchema.fields` growable (dynamic array + count/cap), remove `MAX_OBJ_FIELDS` checks/limits and fixed copies.
2. **Function bindings / scope-local function metadata**: make `Scope.funcs` growable; remove `MAX_FUNCBINDS`.
3. **Captures**: replace fixed `captures[8][...]` with dynamic capture list/count/cap (and dynamic names) in AST/function metadata and closure emission paths.
4. **Parameters/arguments/signatures/overloads/call-site registries**: audit and convert fixed-size registries/tables in parser/codegen (param arrays, overload/signature storage, call-arg tracking, temp vectors) to growable structures.
5. **IR structures** (if any fixed arrays remain in `ir.c`) aligned with parser/codegen dynamic forms.
6. Add stress tests for many params/fields/captures/overloads/args and validate.

**Risks:** ABI/shape assumptions in codegen (loops, indexing, emitted argument/capture marshaling), performance regressions.

### Phase 5 — Make generated runtime limits dynamic

**Objective:** Remove user-visible truncation in emitted program runtime (not only compiler internals).

**Steps:**
1. Convert emitted exception state fields to dynamic strings/growable buffers:
   - exception type/message/value/trace/function name
   - stack-trace construction/appending helpers (replace bounded `snprintf`/fixed arrays)
2. Make function-call trace stack growable (replace `LTNG_FN_MAX` fixed arrays) or support dynamic expansion while preserving push/pop semantics.
3. Audit emitted helper temporaries/formatting paths for bounded buffers that can truncate user-visible results (beyond values already made dynamic).
4. Regenerate/compile representative programs and run full suite.

**Risks:** Generated C complexity, memory management correctness in runtime helpers, behavior differences in trace formatting/truncation.

### Phase 6 — Cleanup, constants, and enforcement

**Steps:**
1. Remove or repurpose obsolete `MAX_*` macros once no longer authoritative; keep only intentional, documented limits (if any remain).
2. Replace remaining bounded `snprintf`/fixed-buffer patterns with dynamic-safe equivalents where lengths are unbounded.
3. Address or triage format-truncation/related warnings introduced or left by refactors.
4. Update docs/changelog/status to reflect which limits are dynamic and any intentional remaining constraints.

## Testing and validation strategy

- **Per-change targeted checks**: compile/run the specific feature areas touched (strings, functions/closures, objects/params, arrays, exceptions/traces, reads, large expressions).
- **Regression suite**: `bash development/tests/run_tests.sh` after each phase/substantial substep.
- **Stress/regression additions** (recommended):
  - Very long string literals and runtime string results (concat/repeat/interpolation)
  - Very long identifiers (if language allows) and long member/field names
  - Large numbers of variables/params/fields/captures/overloads/arguments
  - Large expression/source lines and deep nesting
  - Long exception messages/values and deep stack traces
- **Build validation**: native rebuild (`source_code/compiler && make`), and resync multi-arch/universal artifacts via `source_code/builds/build.sh` when compiler behavior/source changes warrant.

## Suggested execution order (incremental commits)

1. Phase 1 (scratch-buffer hardening in parser/codegen, no struct ABI/layout changes)
2. Phase 2 (dynamic `Token.value` + lexer/ownership fixes)
3. Phase 3 (dynamic AST/symbol/object/funcbind string fields)
4. Phase 4 (dynamic collection capacities: fields/funcbinds/captures/params/signatures)
5. Phase 5 (generated runtime trace/exception dynamic buffers)
6. Phase 6 (cleanup/docs/warnings and final validation)

This order minimizes risk by improving buffer safety first, then changing core ownership/layouts, then count-capacity limits, and finally emitted-runtime limits.
