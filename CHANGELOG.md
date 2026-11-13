# Changelog

All notable changes to the Lightning project in this development effort are documented here.

## [Unreleased]

### Added
- `DEVELOPMENT_AUDIT.md`: initial repository and architecture audit, baseline status, risks, and limitations.
- `TESTING.md`: testing infrastructure documentation and conventions.
- `PHASE_STATUS.md`: phase-by-phase implementation and verification tracking.
- `tests/run_tests.sh`: reproducible test runner (builds compiler, runs categorized + corpus + negative tests, executes native binaries).
- Initial Phase 0 test infrastructure directories and smoke tests under `tests/` (lexer/parser/semantic/runtime/codegen/e2e/native/regression/adversarial).
- `tests/runtime/phase1_types_smoke.ltng`: smoke coverage for distinct type reporting and basic conversions.
- Initial support for `tofloat(...)` and `tochar(...)` conversion-function recognition and partial handling in expression/type contexts.
- Support for explicit type declarations `let x = float` and `let x = char` (with default initialization).
- `limitations.md`: consolidated limitations/gaps for Phases 0–3.
- Phase 2 semantic/scope improvements: declarations allowed in control-flow blocks, switch cases, nested blocks, and standalone bare `{ ... }` blocks; variable shadowing support; inline block-local declarations in codegen; scope-aware unique C names for shadowed declarations.
- Phase 3 arrays and collections: dynamic array storage (`char **data` + `len`/`cap` with automatic reallocation); array methods `push`, `pop`, `pop(index)`, `insert`, `remove`, `contains`, `index_of`, `sort`, `reverse`; `length` property; array copy/assignment in `let`/`set` forms (`a`, `a.copy()`, `a.slice(...)`); for-each iteration (`for (x in arr)`); 1-based positive / negative-from-end / 0-invalid indexing preserved; numeric-aware `sort` for fully numeric elements; `write(arr.length)` reports numeric length; methods usable in `write()` and as statements.
- Intermediate representation (IR) pipeline: compiler emits `<source basename>.ltng_ir` after successful parsing and removes it after successful check/emit/compile when `--debug` is not used; with `--debug`, creates `debug/<source basename>/` containing `source.ltng`, `intermediete.ltng_ir`, and `emitted.c` (when applicable).

### Changed
- Type tracking now distinguishes float values/literals as `TYPE_FLOAT` in key parsing/inference paths (previously merged to int in several cases).
- Type name reporting updated to use distinct `float` and `char` names (previously reported as `int`/`string` in merged representations).
- Assignment type compatibility adjusted to allow numeric (`int <-> float`) and `char <-> string` compatibility for backward-compatible behavior.
- Updated `code/t_null_set_type_change.expected` to expect `float` typeof for `3.14` (distinct float typing).

### Fixed
- Improved float variable handling in expression type inference to avoid incorrect type regressions in set/locked-variable checks for existing numeric cases.
- Compiler now returns a non-zero exit code when backend C compilation fails (instead of reporting failure and exiting successfully in some paths).
- Fixed object-literal `set` assignments to emit update semantics (not redeclarations) compatible with block-scope inline declaration behavior.
- Corrected declaration/reference handling for block-scoped and shadowed variables in key parse/codegen paths.
- Addressed several compiler-source warnings in feasible paths (unused functions/parameters/variables, missing switch/default handling in key cases, label/declaration formatting issue, embedded-NUL format issue, const-correctness in helper path, and increased output/path buffer sizing in `main.c`); format-truncation warnings remain in complex expression/codegen formatting paths.

### Compatibility / Breaking changes
- `typeof` results are more precise for float values in covered cases (e.g. `3.14` now reports `float` instead of `int`). Existing programs/tests relying on previous merged reporting may need updates.

### Notes
- Work is in progress across planned phases; see `PHASE_STATUS.md` for current verification status and remaining limitations.
- Existing repository content and untracked user artifacts (e.g. `code/compiled/`) preserved.
