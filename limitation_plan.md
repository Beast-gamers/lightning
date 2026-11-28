# Remaining Limitations Plan (Easiest → Hardest)

## Tier 1 — Easy (localized tests/docs/small guards) — DONE
1. [x] **Conversion edge-case coverage gaps** - Add more runtime/compile-time tests for remaining permutations (nested conversions, unusual contexts, `tobool`/`tochar` edge inputs) and tighten docs; low risk, mostly tests.
2. [x] **Div/mod unusual emitted-form coverage** - Add targeted tests/fixes for any remaining weird emitted-expression shapes (beyond already-covered compound/complex subexpressions); small codegen/parser guards if found.
3. [x] **Typed-catch ergonomics/docs edge cases** - Small improvements around exact-match behavior documentation/tests (e.g., whitespace/type name edge cases, catch-all ordering expectations); no semantics redesign.
4. [x] **Diagnostic wording improvements (targeted)** - Improve specific confusing messages (parse/catch/conversion/type mismatch) without redesigning diagnostics system.
5. [x] **Limitations/PHASE_STATUS drift cleanup** - Keep counts/caveats precise as tests change; trivial but important.

## Tier 2 — Medium (contained compiler/codegen work) — IN PROGRESS
6. [x] **More provable type-compatibility checks** - Extend conservative compile-time rejection for obvious mismatches in more contexts, while preserving dynamic pass-through behavior.
7. [x] **Return-type/path validation hardening** - Cover more provable annotated-return mismatch/control-flow cases; avoid false positives in dynamic code.
8. **Sole-call typing inference gaps (partial)** - Reduce conservative string fallback in more sole-call/propagation paths without breaking mixed/unknown cases.
9. **Copy/slice general expression-context gaps (partial)** - For-in `arr.copy()/slice()` supported via temp arrays; other expression-context gaps remain within non-first-class array-value limits.
10. **Base-param UDM array-receiver detection gaps (partial)** - Pre-scan now detects any `base.method(...)` call; other unrecognized patterns remain.
11. [x] **Exception model ergonomics** - e.g., better catch-type matching utilities/patterns or clearer runtime error typing; still without inheritance/user-defined exception classes.

## Tier 3 — Hard (cross-cutting semantics/runtime)
12. **`+` add/concat ambiguity** - Making mixed string/numeric `+` deterministic and intuitive without breaking legacy behavior is semantically risky.
13. **Heuristic typing/coercion cleanup** - Requires careful, widespread behavior changes and compatibility strategy.
14. **Arrays not fully first-class / expression-value limits** - Needs broader value-model/codegen changes across many contexts.
15. **Exception object model depth** - First-class exception instances, custom fields/classes/inheritance are major runtime/type-model work.
16. **Runtime/compile-time memory audit & ownership strategy** - Comprehensive ownership/lifetime model and leak/audit coverage across compiler+generated runtime.
17. **Rotating/static return-buffer aliasing/lifetime constraints** - Fundamental to current runtime representation; safe redesign is invasive.

## Tier 4 — Very hard / architectural / env-limited
18. **Arrays of objects / jagged dynamic arrays (first-class)** - Requires major runtime collection/value-model redesign.
19. **Unified automatic memory management / GC-like cleanup** - Large runtime + codegen redesign with compatibility/perf risks.
20. **Full memory/tooling validation** - Valgrind unavailable and ASAN unusable in this proot/container env; comprehensive sanitizer/leak-stress/fuzz proof needs compatible environment/CI.
