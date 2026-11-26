# Static / Fixed Limits Audit

Audit date: 2026-04-12 (post `0d7706b`)

Scope: `source_code/compiler/source/src/*` and emitted runtime patterns in `codegen.c`. This is a current snapshot after the parser_expr/parser_sym/codegen hard-limit cleanup passes.

## Summary

No obvious **hard truncation limits** were found in compiler source by static scan:

- No `strncpy`/`strncat`/`sprintf`
- No fixed `char[N]` source buffers
- No active `MAX_*` / `CG_MAX_*` / `LTNG_MAX_*` macros
- No `65535` truncation guards

Full test suite baseline at this snapshot: **PASS=183 FAIL=0 SKIP=0**.

## Remaining non-truncating fixed/initial capacities

These are initial heap sizes, growth floors, or IO chunk sizes. They are acceptable when growable or when they do not truncate user content, but should stay on the audit radar.

| File | Examples | Notes |
|---|---|---|
| `parser_sym.c` | `2048/4096/8192` floors in normalize paths; `cap < 4096` in optimize path | Initial/growable floors; normalization buffers grow/realloc. |
| `parser_expr.c` | `65536` initial expr/check buffers; `256` CE_GROW step | Initial growable capacities; collect/expr APIs grow. |
| `parser_stmt.c` | `65536` initial scratch caps; `16384` prefix initial cap; `4096` final_expr initial cap | Initial growable capacities; many paths use `SNPRINTF_FULL`/growable cexpr APIs. |
| `codegen.c` | `65536` expr_out initial caps; small 2-byte temp allocs | expr_out buffers passed to growable parse APIs; initial caps only. |
| `main.c` | `4096` read/copy chunks; `4096` initial compiler_err_cap | Chunk IO is not content truncation; compiler_err grows. |
| `lexer.c` | `4096` initial token capacity; `64` initial string token buffer cap | Both grow/realloc. |
| `errors.c` | `128` initial source_lines_cap | Grows dynamically. |

## Bounded API / refusal cases (not silent truncation)

| File | Location/pattern | Notes |
|---|---|---|
| `parser_sym.c` | `mangle_name`, `field_mangled_name` write into caller `out_size` | Uses bounded copy; callers currently provide adequate sizes. Consider growable/alloc-only API if call sites become uncertain. |
| `codegen.c` | `cg_rewrite_divmod` measures formatted output and returns `0` if it does not fit `outsz` | Avoids silent truncation but can refuse rewrite; callers should pass growable buffers where possible. |
| `parser_sym.c` | `optimize_add_concat` grows internal result, but final write-back to `out` only occurs if it fits `out_size` | Avoids overflow; caller-owned fixed buffer remains an API constraint. |

## Generated runtime code patterns (emitted C, not compiler-source buffers)

| File | Pattern | Notes |
|---|---|---|
| `codegen.c` | emitted runtime helpers use `strcat` into `malloc(total)`-sized buffers | Safe by construction; emitted code, not compiler scratch buffer. |
| `parser_expr.c` | emitted `snprintf(%s, sizeof(%s), ...)` for some string inc/dec/update paths | Runtime/generated-code pattern; may be a real limit if target is not a fixed char array in generated C. Review semantics before changing. |

## Audit checklist

- [x] Remove `strncpy/strncat/sprintf` from compiler source.
- [x] Remove fixed `char[N]` source buffers.
- [x] Remove active `MAX_*` hard-limit macros from `src`.
- [x] Re-scan `parser_expr.c`, `parser_sym.c`, `codegen.c`, `main.c`, `lexer.c`, `errors.c`.
- [ ] Continue converting remaining initial fixed caps to clearly growable/justified sizes where input-derived.
- [ ] Review generated C runtime `sizeof(...)` snprintf patterns for pointer-target safety.
- [ ] Consider growable APIs for caller-bounded helpers (`mangle_name`, divmod rewrite/callers) if needed.
