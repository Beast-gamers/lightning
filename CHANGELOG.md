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
- Phase 2 semantic/scope improvements: declarations allowed in control-flow blocks, switch cases, and nested blocks; variable shadowing support; inline block-local declarations in codegen; scope-aware unique C names for shadowed declarations.

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

### Compatibility / Breaking changes
- `typeof` results are more precise for float values in covered cases (e.g. `3.14` now reports `float` instead of `int`). Existing programs/tests relying on previous merged reporting may need updates.

### Notes
- Work is in progress across planned phases; see `PHASE_STATUS.md` for current verification status and remaining limitations.
- Existing repository content and untracked user artifacts (e.g. `code/compiled/`) preserved.
