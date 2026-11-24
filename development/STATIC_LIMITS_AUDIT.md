# Static / Fixed Limits Audit

Audit date: 2026-04-11 (post `dda68e6`)

Scope: `source_code/compiler/source/src/*` and emitted runtime patterns in `codegen.c`. This lists **current fixed/static limits** found by code scan; it is intended as a follow-up backlog for the “every limit is dynamic” effort. Not every item is equally severe, but each is a fixed bound/capacity/stack buffer that can truncate, reject, or misbehave on large inputs.

## 1) Remaining fixed-limit macros / bounds

| File | Line | Limit | Notes |
|---|---:|---:|---|
| `codegen.c` | 8 | `CG_MAX_CLEANUPS 128` | Max cleanup slots tracked by codegen; overflow drops cleanup tracking. |
| `codegen.c` | 9 | `CG_MAX_LOOPS 64` | Max loop nesting/depth tracked in `g_loop_bases[CG_MAX_LOOPS]`. |
| `parser_stmt.c` | 670 | `MAX_IDX_W 32` | Max array-index depth in a write/index parsing path; uses fixed `calloc(MAX_IDX_W,1024)`. |

## 2) Fixed-size heap scratch buffers (fixed cap, not growable)

These are heap-allocated but with fixed sizes; large inputs can truncate or fail. Many were converted from stack macros but still have fixed capacities.

### `parser_sym.c` (string normalization / type resolution)
| Line(s) | Size | Notes |
|---|---:|---|
| 158 | `cap = 256` initial, grows | `obj_build_all` is dynamic/growable (OK). |
| 930,1034 | 2048 | Temp/result buffers in `normalize_string_expr`/concat normalization; fixed. |
| 934 | 512/512 | `left/right` fixed buffers. |
| 1055 | 512 | `cntbuf` fixed. |
| 1111-1118 | 8192/4096/4096/4096/2048/4096/4096/1024 | Main normalization work buffers; fixed with `strncpy`/size caps. |
| 1123-1124,1203-1205,1393-1395,1459 | 8192 | Work buffer copy/formatting limited to 8192. |
| 1214,1237-1238,1283,1389,1415,1417,1444-1459 | 4096/2048/1024 | Multiple fixed parse/normalize slices. |
| 1511-1513,1522,1531,1545 | 4096 | `normalize_add_expr` fixed buffers. |
| 1601 | 4096 | Interp expression inner buffer. |

### `parser_expr.c`
| Line(s) | Size | Notes |
|---|---:|---|
| 13 | `vcap = 4096` initial | `parse_inline_array` values buffer is `calloc(1, vcap)`; grows? check: appears fixed initial; should be growable. |
| 79 | 65536 | `val` buffer for inline array element parsing. |
| 130 | 65536 | Expression buffer. |
| 415,438,476 | 4096 | `left/right/ltmp/rtmp/lt/rt` fixed buffers. |
| 1338 area (inner_c) | 65536 initial, growable via API | `parse_expression_from_string` now grows; caller cap grows (OK). |
| 3490 | 65536 initial, growable | `check` cap grows (OK). |

### `parser_stmt.c`
| Line(s) | Size | Notes |
|---|---:|---|
| 670-676 | `MAX_IDX_W 32`, `1024` per index | Fixed max index width and fixed per-index expr buffer. |
| many | 65536 | Numerous heap scratch buffers allocated at fixed 65536; most are not growable unless passed through growable API. |
| many | 256/528 | Name/cname scratch buffers fixed at 256/528. |

### `codegen.c`
| Line(s) | Size | Notes |
|---|---:|---|
| 145-147 | 128 | Arg label buffer fixed to 128. |
| 265,331,390,456 | 65536 initial | `expr_out` buffers; now passed to growable `parse_expression_from_string` (cap can grow) — improved, but initial fixed. |
| 595-597 | `outsz or 8192` | Div/mod rewrite lhs buffer. |
| 760,769,785,906,913,1055,1064,1080 | 4096 | `_rex` rewrite buffers; fixed 4096 cap passed to `cg_rewrite_divmod`. |
| 1095,1420 | 64 | Temp buffers for names/formatting. |
| 1443,1522 | 256 | `top_expr` snprintf fixed to 256. |
| 2080 | comment | States dynamic helpers (not a limit). |

### `main.c` (CLI/path/IO)
| Line(s) | Size | Notes |
|---|---:|---|
| 10,13 | 4096 | `mkdir_p` path buffer; paths >=4096 rejected. |
| 32,34,79,213,229,246,262,282,320,322,340,365,371,373,431 | 4096 | File copy/log/read buffers and fixed-size IO chunks. Fixed buffer size is OK for chunked IO, but often paired with fixed path/string assumptions. |
| 101 | 32 | Timestamp buffer. |
| 152-157,161-167,217-218,291-298 | 4096 | Debug/source/IR/C/log/output path buffers; fixed 4096 path limit. |
| 250 | 512 | Temp C path buffer. |
| 344,346,348 | 4096 | Compiler command buffer; long paths/flags can truncate. |
| 370,373,378 | 4096 | `compiler_err` fixed 4096 with `strncat`; compiler error output can be truncated. |
| 390-401 | 4096/8192 | Debug artifact paths and debug command buffers. |

### `lexer.c`
| Line(s) | Size | Notes |
|---|---:|---|
| 37-38 | 4096 initial | Token array initial capacity; dynamic growth exists (not a hard limit, but initial fixed). |
| 443 | 64 | Error message buffer fixed. |

### `errors.c`
| Line(s) | Size | Notes |
|---|---:|---|
| 20 | 128 initial | `source_lines_cap` starts at 128, grows dynamically (initial fixed only). |

## 3) Fixed runtime/ABI/structural limits to audit further

These were not fully proven fixed by this quick scan, but should be checked carefully (the old limitations list mentioned them):

| Area | Status | Notes |
|---|---|---|
| `MAX_VARS`, `MAX_ERRORS`, `MAX_FUNCS`, `MAX_FUNC_PARAMS`, `MAX_OBJ_FIELDS`, `MAX_FUNCBINDS`, `MAX_OBJS` | Macro names removed/unreferenced in `src/` | Verify corresponding struct/array capacities are truly dynamic. |
| `LTNG_FN_MAX` runtime func stack | Not found as macro in `src/` | Check codegen emitted runtime and runtime structs for fixed function-stack depth. |
| Closure captures | No `captures[8]` found | Verify capture storage/count is dynamic in AST/codegen/runtime. |
| Fixed struct arrays in `ltng.h` | No obvious `[N]` fields found by quick scan | Recheck carefully; some limits may be represented as caps/ints with fixed allocs. |

## 4) Recommended next fixes (highest value)

1. **`parser_sym.c` normalization buffers** (4096/8192/1024/512): make growable/dynamic; these can truncate complex expression normalization.
2. **`main.c` path/command/error buffers** (4096/8192): make dynamic paths and command construction; avoid `strncat` fixed error buffer.
3. **`codegen.c` `_rex` 4096 rewrite buffers**: make `cg_rewrite_divmod`/callers dynamic.
4. **`parser_stmt.c` `MAX_IDX_W 32` / 1024 index expr buffers**: make dynamic.
5. **`CG_MAX_CLEANUPS`/`CG_MAX_LOOPS`**: convert to growable arrays.
6. **Audit remaining 65536 scratch buffers**: convert to growable where user input can exceed cap; keep fixed only where provably safe.

## 5) Notes

- “Fixed 65536 heap buffer” is better than a compile-time macro limit, but still a fixed cap unless grow/retry logic exists.
- This audit was produced by static scanning and spot inspection; line numbers may shift as code changes.
- Full test suite was green after recent dynamic-limit changes: `PASS=183 FAIL=0 SKIP=0`.
