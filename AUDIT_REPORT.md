# Lightning (ltng) Audit Report

## Mission
Exhaustive bug audit, edge-case testing, and stabilization of the EXISTING
implementation. No new features, no syntax changes, no redesign. Everything
reported here is supported by actual test execution.

## Components inspected
- compiler/src/ltng.h          - types, token types, AST, scope, declarations
- compiler/src/lexer.c         - lexer/tokenizer
- compiler/src/parser.c        - parser, expression collector, type inference
- compiler/src/codegen.c       - C code generation + runtime helpers
- compiler/src/errors.c        - error state/reporting
- compiler/src/main.c          - compiler driver (compile/check/emit-c)
- bolt                         - CLI driver (compile-and-run wrapper)
- compiler/ltng                - rebuilt compiler binary (now reflects fixes)
- code/*.ltng (83 files)       - existing test programs (regression baseline)

## Implemented feature inventory (from source)
Tokens: WRITE, READ, LET, CONST, SET, LPAREN, RPAREN, EQUALS, STRING, CHAR,
INT_LIT, FLOAT_LIT, IDENT, NEWLINE, EOF, ERROR, PLUS, MINUS, STAR, SLASH,
PERCENT, PLUS_PLUS, MINUS_MINUS, PLUS_EQUALS, MINUS_EQUALS, STAR_EQUALS,
SLASH_EQUALS, PERCENT_EQUALS, AMP_AMP, PIPE_PIPE, CARET_CARET, BANG, EQ_EQ,
BANG_EQ, LT, GT, LT_EQ, GT_EQ, IF, ELSE, SWITCH, CASE, DEFAULT, BREAK,
CONTINUE, WHILE, DO, FOR, FROM, TO, STEP, FUNCTION, RETURN, COMMA, DOT, COLON,
LBRACE, RBRACE.
Keywords: let const set write read if else switch case default break continue
while do for from to step function return and or not true false t f int string
bool obj typeof.
Comments: # to EOL. Whitespace: space/tab/CR skipped; newline is a token.
String escapes: \n \t \\ \" \' \0. Angle-bracket literal quoting <...> inside
strings (brackets removed by codegen).
Data types: TYPE_INT, TYPE_FLOAT, TYPE_STRING, TYPE_CHAR, TYPE_BOOL, TYPE_OBJECT,
TYPE_NULL (float and char merged into int/string at analysis; all variables are
char buffers in generated C).
AST nodes: NODE_WRITE, READ, LET, CONST, SET, IF, ELSE, SWITCH, CASE, BREAK,
CONTINUE, WHILE, DO_WHILE, FOR, FUNCTION, RETURN, CALL, PARAM.
Statements: write(), read(), let, const, set, return, function, if/else/else if,
while, do-while, for (from..to step +|-|*|/), switch/case/default, break, continue,
++/-- (stmt & expr), compound assignment (+= -= *= /= %=).
Expressions: literals, variables, parens, unary -/!, arithmetic +-*/%, == != < >
<= >=, && || ^^ (logical on bools, bitwise on ints), string concat +, string
repeat *, typeof(), function calls, UFCS receiver.method(args), object literals,
object field access, string interpolation $[...].
Built-ins: write, read, typeof.
Scope: global + block scopes (control blocks/loop bodies disallow let/const),
function scopes, nested functions lifted with unique names (non-capturing,
siblings visible, no forward references).
CLI: ltng [--debug] [--check] [--emit-c] [--cc <c>] <file.ltng> [output];
bolt wrapper with --debug/--check/--compile/--install/--uninstall/--reinstall/--help.
Runtime helpers emitted in C: _ltng_concat, _ltng_repeat, _ltng_retbuf,
_ltng_ad, _ltng_as, _ltng_from_int, _ltng_from_double, _ltng_is_num_str,
_ltng_add_sn.

## Confirmed bugs found and FIXED
All reproduced, root-caused, fixed with the smallest change, and regression-tested.

1. BUG-1 (crash): String concatenation with a numeric operand segfaulted.
   Root cause: `collect_expression` post-processing split `+` expressions into
   parts and wrapped them as `snprintf(_ltc,512,"%s%s", part1, part2,...)`;
   numeric parts (literals, `atoi(...)`, `(-3)`, `++x`, statement expressions)
   were passed directly to `%s` although they are not `char*`.
   Repro: `let ch = 'A'; let ch2 = ch + 1`, `set s = s + 1`,
   `return doubled + 1`.
   Crashed programs: tc_let, tc_edge, test_edge, test_edge2, functions.
   FIX (parser.c): non-string concat parts are converted to their string form
   (`(long long)(<part>)` / `(double)(<part>)`) before the `%s` snprintf;
   statement expressions from `transform_string_multiply` stay as-is.

2. BUG-2 (inconsistency): decimal/negative float literals printed wrong in
   expression context: `write(-3.14)` → `-3` (via `%lld`) while
   `write(3.14)` → `3.14` (raw literal); `write(10.0/3.0)` → `3` though docs
   say `3.33333`.
   FIX (codegen.c): expression print paths (write/set/assign/return) now use
   `%g`/`(double)` when the expression text contains `.`, `%lld`/`(long long)`
   otherwise; fprintf formats use `%%g`/`%%lld` so the generated C gets the
   right specifiers.

3. BUG-3 (inconsistency): `let x = 3.14` stored `atoi("3.14")` = `3` while
   `set x = 3.14` stored `"3.14"` — `let` and `set` disagreed on float
   literals.
   FIX (codegen.c): `codegen_assign` literal branch now stores the raw string
   for INT/FLOAT (matching `set`); BOOL keeps `true`/`false`.

4. BUG-4 (compile failure on valid code): `switch` on a char/string variable
   generated `strcmp(c, 'A')` — a char constant (int) passed to strcmp — so
   the generated C failed to compile.
   FIX (parser.c): char case values in string-typed switches are emitted as
   string literals `"A"`.

Also removed a stray debug `fprintf(stderr, "[DBG744] ...")` from parser.c
that polluted compilation error output.

## Test results
Final harness (/tmp/opencode/final_harness.sh): 38 passed / 0 failed / 38 total
- 6 regression tests for the 4 fixed bugs (rg_* tests)
- 17 new edge-case tests (lexer, vars, control, functions, objects, strings,
  typeof, inc/dec, compound assignment, interpolation, deep nesting)
- 15 error-case tests (must fail compilation), incl. all project test_err_*
  files

Full project regression (83 code/*.ltng + examples/*.ltng): every file that
previously compiled and ran still does so, with only the intentional, corrected
output changes from BUG-2/BUG-3 (float literals now print/stored faithfully,
e.g. `write(10.0/3.0)` → `3.33333`, `write(-3.14)` → `-3.14`,
`let x = 3.14` → `3.14`). All error-test files still fail compilation as
expected. Zero crashes across the entire corpus.

## Files modified
- compiler/src/parser.c  (BUG-1, BUG-4 fixes; debug fprintf removal)
- compiler/src/codegen.c (BUG-2, BUG-3 fixes)
- compiler/ltng          (rebuilt binary)
- AUDIT_REPORT.md        (this report)

## Known non-bugs / out of scope (documented, not fixed)
- Missing language features (e.g. modules, file I/O built-ins, arrays) are not
  bugs — the language is incomplete by design.
- `switch` docs show braced-less fallthrough cases (`case 'A': case 'a':`),
  but the implementation requires `{}` after every case and codegen has no
  fallthrough; rejected with a clear parse error. Fixing would require adding
  fallthrough semantics — out of scope.
- Duplicate top-level function names fail at C compile (redefinition) rather
  than at ltng parse time; error, no crash.
- Accessing an undeclared object field fails at C compile (undeclared field
  variable); error, no crash.
- Long identifiers (>255 chars) are truncated by the lexer; >255-char combined
  identifiers then parse-error. Strings >4096 chars rejected by lexer.
- `t`/`f` and type keywords are reserved (cannot be variable/function names).
- `_ltng_retbuf` uses 32 round-robin 256-byte buffers; >32 simultaneous
  live string results can alias — pre-existing, edge case.
- Behavior quirks inherent to the existing type model (e.g. `let h = fv`
  where fv is bool stores `1`, since bool in expression context = 1/0; `x + x`
  on a float-typed param goes through `_ltng_add_sn` and becomes a string) are
  consistent with the current implementation and docs' type model.

## Remaining verification
None — full corpus passes, zero crashes.
