# Lightning Development Audit

Date: 2026-11-12 (audit performed in `/main/language`)
Scope: Initial codebase audit per mission requirements before implementing enhancement phases.

## 1. Repository overview

- Git repo: yes. Recent commits show incremental feature work (arrays push/pop/set, exit, escape sequences). Working tree has untracked `code/compiled/` (preserved).
- Primary artifacts:
  - `compiler/src/`: C99 compiler sources (`ltng.h`, `lexer.c`, `parser.c`, `codegen.c`, `errors.c`, `main.c`).
  - `compiler/Makefile`: builds `compiler/ltng` with `cc -Wall -Wextra -pedantic -std=c99`.
  - `bolt`: POSIX shell driver for compile-and-run, `--check`, `--compile` (universal packaging), install helpers.
  - `documentation.md`: user-facing language documentation (claims partial implementation; notes missing features).
  - `AUDIT_REPORT.md`: prior stabilization audit (2026-era in file) documenting bug fixes and array work.
  - `code/*.ltng`: 101 source/test programs (mix of demos, feature tests, edge cases, error cases, expected-output pairs for some).
  - `examples/*.ltng`: 4 example programs.
  - `ltng`, `value` (root), `compiler/hello`: prebuilt binaries/artifacts present in repo (kept as-is).

## 2. Existing features and actual implementation status (verified from source/tests)

### Lexer/tokenizer (`compiler/src/lexer.c`)
- Dynamic token array (grows via realloc from initial `MAX_TOKENS`).
- Tokens: keywords/statements (`write`, `read`, `let`, `const`, `set`, `if/else`, `switch/case/default`, `break/continue`, `while`, `do`, `for/from/to/step`, `function/return`, `exit`), literals (string, char, int, float), identifiers, operators (`+ - * / %`, `++ --`, compound assignments, comparisons, `&& || ^^ !`, `and/or/not` aliases), punctuation (`(){}[],:.`), newline, EOF/error.
- String escapes handled in lexer: `\n \t \\ \" \' \0 \< \>`. Unterminated strings and format-specifier `%` usage produce lexer errors. String length capped by `MAX_STRING_LEN` (4096).
- Char literals supported. Float literals require digit after dot.
- Comments `#` to end-of-line. Newlines are significant tokens.
- Identifiers truncated at `MAX_IDENTIFIER_LEN-1`.

### Parser and AST (`compiler/src/parser.c`, `ltng.h`)
- Builds AST nodes and maintains symbol tables/scopes; performs substantial semantic checking during parsing (not a separate semantic pass module).
- Node types include program/statements, control flow, functions/returns/calls/params, array literal/access/push/pop/set, exit.
- Program split into `functions` list and top-level statement list; functions lifted/emitted separately in codegen.
- Expression parsing/collection is custom and string-oriented: builds C-expression fragments into buffers while doing type inference checks. Handles literals, variables, unary ops, binary ops, typeof, calls/UFCS, object literals/field access, array literals and chained indexing, string interpolation validation, string concat/repeat transforms.
- Array implementation (current): fixed-size array literals (1D..nD up to what parser/codegen support; 7D exercised in tests) stored as C arrays of `char[256]` elements (`*_data`) with dimension metadata in symbol table/AST. 1-based positive indexing, negative indexing from end, index 0 invalid. Multi-dim access lowered to linear index helpers (`_ltng_idx2`, `_ltng_idx3` for 2D/3D patterns used). Array elements in expressions/interpolation supported; array+ in numeric contexts routed through `_ltng_add_sn` in some cases.
- Objects: object literals, field access, dynamic-ish handling via mangled field variables and schemas/registry (`obj_register`, `obj_lookup`), UFCS base-parameter conventions, param schema merging at codegen time in places.
- Functions: named functions, parameters, returns, recursion, nested functions lifted with mangled C names and scope-aware resolution; UFCS call forms; call collection/building. Closures/capture not implemented as first-class lexical capture.
- Scopes: hierarchical `Scope` with parent links, local declarations, function bindings per scope, `disallow_decl` for control blocks. `let/const` disallowed in some control-block contexts per current rules; block-scoped behavior exists but differs from target general-purpose semantics.
- Type-related checks: detects basic literal/variable types, enforces some assignment/lock/type-conversion rules (e.g. `x.lock()`/`x.unlock()` semantics present, dtype lock behavior tested), const enforcement, undefined-variable detection in many paths. Distinct int/float typing is partial/inconsistent in representation.
- Control flow: if/else, while, do-while, for-from-to-step, switch/case/default, break/continue, exit.
- Baseline (non-error, excluding lock/type-change error tests by name pattern): 76/76 runnable tests pass with expected outputs where provided. 2 additional files (`t_lock_null.ltng`, `t_lock_type_change.ltng`) are intentional compile-error tests.

### Code generator (`compiler/src/codegen.c`)
- Emits a single C translation unit to temp file (or `--emit-c` target), then invokes C compiler (`cc` by default, `-w -static` in `main.c`; debug shows compiler output).
- Runtime model: almost all values represented as fixed-size `char[256]` buffers/strings in generated code. Numeric operations emitted as C integer/double expressions or via runtime helpers depending on context.
- Emitted runtime helpers include: `_ltng_concat`, `_ltng_repeat`, `_ltng_arr_get`, `_ltng_idx2`, `_ltng_idx3`, `_ltng_retbuf` (32-slot rotating buffer), `_ltng_ad/_ltng_as/_ltng_from_int/_ltng_from_double`, `_ltng_is_num_str`, `_ltng_add_sn` (numeric-add-or-string-concat heuristic).
- Generates code for statements/expressions, control flow, function definitions/calls, arrays/objects, interpolation formatting, input via `read`.
- Contains specialized handling for typeof, interpolation, array element operations, push/pop/set, base-assignment calls, etc.

### CLI and build system
- `compiler/Makefile`: simple build/clean for `ltng`.
- `compiler/src/main.c`: argument parsing (`--debug`, `--check`, `--emit-c`, `--cc`), temp C emission, compile to output (default `compiled/<base>` next to source), check-only mode returns 0 on success.
- `bolt`: wrapper resolving compiler binary, supports `--debug/--check/--compile/--install/...`, run mode compiles to `/tmp` binary and executes with forwarded args (args after source passed through - important for CLI phase), universal compile mode packages per-arch binaries. Truncated tail in some views but full script present.
- On this environment, `compiler/ltng` lacked execute permission after build; copying to `/tmp/opencode` and `chmod +x` allowed execution (noted for testing).

### Tests, examples, documentation
- `code/`: extensive ad-hoc regression/feature corpus (`test_*.ltng`, `tc_*.ltng`, demos, `*_errors.ltng`/`test_err_*`, `.expected` for 8 cases including arrays, locks, typeconv/typeof/tofunc3).
- `examples/`: basic samples.
- `documentation.md`: documents many implemented constructs and notes limitations (incomplete language, array mutability limits historically noted but arrays evolved; some doc/code drift possible).
- No unified automated test runner or structured test directories (lexer/parser/semantic/unit/e2e separation) yet - Phase 0 gap.

## 3. Architecture and dependencies

- Pipeline (actual): Source -> `lexer_tokenize` -> `parse` (AST construction + integrated semantic checks/type inference/symbol tracking) -> `codegen` (C emission) -> external C compiler -> native executable. No separate IR or optimizer module.
- Single-process compiler binary in C99, standard library only. External dependency at runtime/build-exec: system C compiler (`cc/gcc`), optional zig/cross-compilers used by `bolt --compile` universal packaging.
- Symbol table: scoped linked structure (`Scope`/`SymbolTable`) with fixed arrays for vars/func bindings in scope structs plus global registries for functions/objects/param schemas in parser/codegen.

## 4. Known bugs, issues, and limitations (initial)

- **Type system:** int/float/bool/char/null largely normalized to string-buffer runtime representation; distinct numeric semantics, precision, promotion, and explicit conversions not fully aligned with target general-purpose behavior.
- **Scopes/declarations:** declaration rules tied to control-block restrictions in places; semantics for shadowing/lifetimes/nested blocks need verification against target (and must not break existing behavior).
- **Arrays:** currently fixed-size literal-based structures in practice; required dynamic array API (push/pop/insert/remove/set/get/length/contains/index_of/sort/reverse/slice/copy/iteration semantics), homogeneity/mutation/aliasing semantics, and robust bounds/error behavior need implementation/definition. Must preserve Lightning indexing (1-based, negative from end, 0 invalid).
- **Exceptions:** try/catch/finally/throw/custom errors/propagation/stack traces not implemented.
- **Function system:** closures/captures, anonymous functions, function references/values, default/named/variadic args, overloading, full return-type checking not fully present.
- **Objects:** methods/iteration/copy/equality/delete/field-existence checks and dynamic mutation semantics incomplete relative to target.
- **Operators/loops:** bitwise/shift/compound-bitwise, null-coalescing, ternary, for-in need evaluation/implementation with defined semantics.
- **Standard library/built-ins:** many requested math/string/array/conversion utilities missing or incomplete; argument validation/memory behavior inconsistent.
- **CLI runtime features:** command-line args exposure inside language, env vars, stdio/stderr distinctions, cwd/process/OS/time/sleep/exec/child process APIs largely not exposed as language/runtime features (bolt forwards args to executables but language-level access must be verified/implemented).
- **Diagnostics:** basic file/line/col/source/caret messages exist; error codes, warnings taxonomy, suggestions quality, and runtime stack traces are limited.
- **Memory management/ownership:** heavy reliance on fixed 256-byte buffers, stack-like C locals, static rotating buffers (`_ltng_retbuf`, add buffers) causing aliasing/lifetime hazards; dynamic allocations in compiler (`malloc/realloc/strdup`) not all systematically audited/freed in all paths; runtime string/array/object/closure lifetimes need a consistent ownership strategy.
- **Compiler robustness:** parser/codegen use fixed-size formatting buffers and have warning-prone paths (observed `-Wformat-truncation`, switch on `TYPE_ARRAY` default handling, unused functions, etc. in build). Expression collection is complex and can be fragile for new constructs.
- **Generated C safety:** `-w -static` hides warnings in normal build path; generated code uses `strcpy/strcat/snprintf` patterns in helpers but many user/runtime values constrained to 256 bytes - overflow/truncation risks need audit.
- **Behavioral quirks:** dynamic typing heuristics (`_ltng_add_sn`, numeric-string detection) can produce surprising results; some docs/tests encode current-model behavior that may conflict with target semantics (compatibility risk).

## 5. Memory-management strategy (current)

- **Compiler:** AST nodes allocated with `malloc` and linked; lists freed in `codegen_free` (needs verification for completeness across all node-owned dynamic fields like `array_dims`, nested lists in all cases). Token array dynamically allocated/freed (`lexer_free`). Source/error storage uses `strdup` and line slicing; some allocations may persist for process lifetime. Scopes contain fixed-size arrays and pointer-linked parents (stack or heap depending on usage).
- **Runtime (generated):** no general-purpose heap allocator for language values. Values are fixed `char` buffers (commonly 256 bytes), function return buffers, and static rotating temporary buffers (limited concurrency/liveness). Arrays are fixed C arrays of fixed element buffers sized from literals/dimensions known at compile/codegen time in current model. Objects mapped to mangled variables/fields. This is incompatible with true dynamic resizing/lifetimes/closures without redesign of runtime representation.

## 6. Compiler and runtime issues to watch

- Exit-code correctness for compile failures (main returns 1 on errors; must ensure all failure paths consistent).
- `bolt` argument forwarding and temp executable behavior (must continue to work).
- Const/lock enforcement and type-change rules (existing tests rely on them).
- Non-capturing nested function model vs future closures.
- Array indexing philosophy must remain (1-based, negative, 0 invalid).
- Potential C identifier collisions despite mangling in complex generated code.
- Large/edge inputs can hit fixed limits (`MAX_STRING_LEN`, tokenization, 256-byte runtime cells, rotating temp buffers).

## 7. Test coverage (initial)

- Strengths: broad set of language-feature and edge-case `.ltng` programs in `code/`; some `.expected` output comparisons; prior audits added regression coverage for arrays and specific bugs.
- Gaps: no organized test harness/framework, limited parser/lexer/semantic unit tests, limited negative-test assertions beyond compile-fail exit in ad-hoc checks, no systematic native-executable validation matrix, no sanitizer/fuzz/memory-leak automation, no differential/metamorphic suites.

## 8. Compatibility risks

- Strengthening type system (distinct int/float), changing implicit coercions, or altering expression semantics can break existing programs/tests relying on current stringly/dynamic behavior.
- Making arrays truly dynamic and changing mutation/length semantics may affect current fixed-array assumptions.
- Scope rule changes (e.g. allowing declarations in control blocks, altering shadowing/lifetimes) must preserve working code.
- Exception/control-flow additions (`try/catch/finally/throw`) introduce new keywords/syntax interactions.
- Function system extensions (closures, defaults, overloading, refs) may require ABI/runtime representation changes.
- Built-in/stdlib naming and behavior must remain consistent with existing usage.
- Documentation currently mixes implemented and aspirational/partial notes; updates must be careful and accurate.

## 9. Previously reported issues - verification plan/status

From `AUDIT_REPORT.md` and mission list: numeric type separation (partially addressed for some print/assign cases but not fully distinct), string equality (present in expression handling; needs systematic verification), fixed-size limits (still pervasive), variable scopes (hierarchical but constrained), nested function capture (not implemented), compiler exit codes (generally correct on observed failures; needs audit), memory leaks (not comprehensively audited/fixed). These will be verified with targeted tests during relevant phases rather than assumed fixed.

## 10. Immediate next steps

1. Phase 0: establish reproducible testing infrastructure and baseline harnesses (lexer/parser/semantic/runtime/codegen/e2e/native-executable/regression) and document in `TESTING.md`.
2. Proceed sequentially through Phases 1-13 with implementation, tests, and documentation, preserving backward compatibility and existing behavior wherever possible.

## 11. Unavoidable limitations noted at audit time

- Current runtime representation (fixed 256-byte string cells, static temp buffers) is a fundamental constraint for several target features (dynamic collections, safe closures/values, large strings/data, robust memory ownership). Addressing phases 3/5/11 will likely require significant runtime/codegen redesign while maintaining compatibility.
- Full "professional" general-purpose semantics may require carefully defined deviations from current heuristic behavior; any breaking change must be documented and minimized.
