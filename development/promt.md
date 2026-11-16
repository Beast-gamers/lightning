Lightning — Professional General-Purpose Language Development

Repository: https://github.com/beast-gamers/lightning/

Act as a senior compiler engineer, programming language architect, runtime engineer, and QA specialist.

Your mission is to evolve the existing Lightning language into a reliable, professional, general-purpose CLI programming language, preparing it for future file parsing and library support.

Do not just create a plan. Actually implement, test, debug, and stabilize the codebase.

1. Engineering rules

- Thoroughly inspect the existing codebase before modifying anything.
- Verify existing functionality directly from source code.
- Preserve existing syntax, features, and backward compatibility wherever possible.
- Never replace working functionality with simplified implementations.
- Never introduce placeholders, fake implementations, or unnecessary dependencies.
- Fix root causes rather than hiding errors.
- Never skip failing tests or weaken assertions.
- Test generated native executables, not just compilation.
- Never claim something works without executing appropriate tests.
- Preserve existing user changes and uncommitted work.
- Do not reset, clean, or force-push the repository.
- Document unavoidable limitations and breaking changes.

2. Initial audit

Inspect:

- Lexer and tokenizer.
- Parser and AST.
- Semantic analysis.
- Code generator.
- Runtime.
- Memory management.
- CLI and build system.
- Tests, examples and documentation.

Create "DEVELOPMENT_AUDIT.md" documenting:

- Existing features and actual implementation status.
- Architecture and dependencies.
- Known bugs and limitations.
- Memory-management strategy.
- Compiler and runtime issues.
- Test coverage.
- Compatibility risks.

Verify previously reported issues, including numeric type separation, string equality, fixed-size limits, variable scopes, nested function capture, compiler exit codes and memory leaks.

Do not assume previous audit reports are completely accurate.

3. Implementation phases

Implement these phases sequentially.

Phase 0 — Testing infrastructure

Establish a reproducible baseline.

Create or improve:

- Lexer tests.
- Parser tests.
- Semantic tests.
- Runtime tests.
- Code-generation tests.
- End-to-end tests.
- Native executable tests.
- Regression tests.

Create "TESTING.md".

Phase 1 — Type system

Implement:

- Distinct int and float types.
- String, char, bool, null, array and object types.
- Type inference.
- Explicit conversions.
- Numeric promotion.
- Type compatibility checking.
- Return-type validation.
- Overflow and division semantics.

Test precision, boundary values, invalid conversions, mixed expressions and null behavior.

Phase 2 — Semantic analysis and scopes

Implement:

- Global scope.
- Function scope.
- Block scope.
- Nested scopes.
- Variable shadowing.
- Duplicate declaration detection.
- Undefined variable detection.
- Constant enforcement.
- Variable lifetime rules.

Allow declarations inside loops, conditionals, switch cases and nested blocks.

Prevent accidental generated C variable collisions.

Phase 3 — Arrays and collections

Implement:

- Dynamic resizing.
- push, pop, insert, remove, set, get.
- length, contains, index_of.
- Sorting and reversing.
- Slicing and copying.
- Iteration.
- Nested arrays.
- Arrays of objects.

Preserve Lightning's indexing philosophy:

- Positive indexing starts at 1.
- Negative indexing starts from the end.
- Index 0 is invalid.

Define array homogeneity and mutation semantics consistently.

Test empty arrays, invalid indices, large arrays, aliasing, nested mutation and memory cleanup.

Phase 4 — Exception handling

Implement:

- try.
- catch.
- finally.
- throw.
- Custom errors.
- Error propagation.
- Stack traces.

Define behavior for exceptions inside nested functions, returns, break, continue, finally and cleanup.

Ensure finally executes correctly before control leaves the construct.

Phase 5 — Function system

Implement as appropriate:

- Anonymous functions.
- Closures.
- Function references.
- Default parameters.
- Named arguments.
- Variadic arguments.
- Function overloading.
- Return-type checking.

Closures must correctly capture variables and preserve their lifetimes.

Test recursion, nested closures, captured mutable variables, independent instances and invalid arguments.

Phase 6 — Object system

Improve:

- Nested objects.
- Dynamic field assignment.
- Field deletion.
- Field existence checking.
- Object iteration.
- Object copying and equality.
- Object methods.

Preserve existing object syntax.

Evaluate classes and constructors only if justified by the language's design.

Phase 7 — Operators and iteration

Implement appropriate:

- Bitwise operators.
- Shift operators.
- Compound bitwise assignments.
- Null-coalescing.
- Ternary expressions.
- for-in loops.

Define precedence, associativity, type restrictions and short-circuit behavior.

Phase 8 — Standard runtime

Implement consistent built-ins.

Math:
"abs", "sqrt", "pow", "round", "floor", "ceil", "min", "max", "random".

Strings:
"split", "join", "replace", "trim", "substring", "contains", "starts_with", "ends_with", "index_of".

Arrays:
"length", "push", "pop", "insert", "remove", "sort", "reverse".

Conversions:
"typeof", "toint", "tofloat", "tostring", "tobool", "tochar".

Ensure proper argument validation, return types and memory management.

Phase 9 — CLI capabilities

Implement:

- Command-line arguments.
- Argument count.
- Process exit codes.
- Environment variable access.
- Standard input/output/error.
- Current working directory.
- Process information.
- OS information.
- Time utilities.
- Sleep.
- External command execution.
- Child process management where practical.

Ensure "bolt" correctly forwards arguments to generated executables.

Handle spaces, quotes, missing arguments, command failures and resource cleanup.

Avoid unsafe shell command construction.

Phase 10 — Compiler architecture

Improve the compiler pipeline:

Source → Lexer → Parser → AST → Semantic Analyzer → IR → Optimizer → C Generator → Native Executable.

Introduce IR if beneficial.

Implement safe optimizations where justified:

- Constant folding.
- Constant propagation.
- Dead-code elimination.
- Common subexpression elimination.
- Unreachable-code detection.

Verify optimized and unoptimized programs produce equivalent results.

Phase 11 — Memory management

Audit all allocations and cleanup paths.

Investigate:

- Strings.
- Arrays.
- Objects.
- Closures.
- Function values.
- Temporary expressions.
- Runtime errors.
- Early returns.

Choose a consistent ownership strategy.

Prevent:

- Memory leaks.
- Use-after-free.
- Double-free.
- Dangling references.
- Buffer overflows.
- Invalid memory access.

Use available sanitizers and memory-debugging tools.

Phase 12 — Diagnostics

Implement professional diagnostics with:

- Error codes.
- Warnings.
- File names.
- Line and column numbers.
- Source excerpts.
- Caret indicators.
- Helpful suggestions.
- Runtime stack traces.

Ensure compilation failures return nonzero exit codes.

Phase 13 — Final stabilization

Perform a complete regression, compatibility, security and adversarial testing campaign.

Do not implement file parsing or library support during this task.

4. Adversarial and edge-case testing

Actively attempt to break every feature.

Test these categories:

Boundary cases

- Zero, negatives, maximum and minimum values.
- Empty inputs and extremely large inputs.
- Deep nesting and long identifiers.
- Large arrays and strings.

Parser cases

- Missing brackets and parentheses.
- Invalid operators.
- Unexpected EOF.
- Malformed declarations.
- Invalid expressions.
- Repeated operators.

Semantic cases

- Undefined variables.
- Duplicate declarations.
- Invalid assignments.
- Illegal scope access.
- Invalid function arguments.
- Incorrect return types.

Runtime cases

- Division by zero.
- Integer overflow.
- Infinite recursion.
- Stack overflow.
- Invalid indexing.
- Invalid conversions.
- Excessive allocations.

Feature interaction

- Closures capturing arrays.
- Nested objects inside arrays.
- Exceptions inside recursive functions.
- Shadowing inside loops.
- Nested try/catch.
- Functions returning closures.
- Object mutation inside nested functions.

Advanced testing

- Metamorphic testing.
- Differential testing against equivalent C/Python implementations.
- Random valid and invalid program generation.
- Lexer/parser fuzzing.
- Resource exhaustion testing.
- Memory-safety testing.

Minimize discovered failures and add permanent regression tests.

The compiler must not crash, enter infinite parser loops, corrupt memory or silently accept malformed programs.

5. Bug-fixing protocol

For every discovered bug:

1. Reproduce it.
2. Minimize the failing example.
3. Identify the root cause.
4. Fix the underlying implementation.
5. Add a regression test.
6. Run relevant tests.
7. Run the complete regression suite.
8. Check for regressions.
9. Update documentation.

Never hide failing tests.

If a fix cannot be verified, document it as unresolved.

6. Compatibility and documentation

Preserve existing Lightning syntax and behavior wherever possible.

Maintain:

- "documentation.md"
- "CHANGELOG.md"
- "TESTING.md"
- "DEVELOPMENT_AUDIT.md"
- "PHASE_STATUS.md"

Document every new feature, including syntax, examples, invalid usage, edge cases, runtime behavior and limitations.

7. Phase completion requirements

A phase is complete only when:

- Implementation works.
- Existing features still work.
- Positive and negative tests pass.
- Boundary and interaction tests pass.
- Regression tests pass.
- Memory checks pass where available.
- Native executables behave correctly.
- Documentation is updated.
- No known critical or high-severity defects remain.

Record implemented features, changed files, test results, discovered bugs and remaining limitations in "PHASE_STATUS.md".

Commit completed milestones when Git configuration permits.

If interrupted, save the exact progress and resume from the last verified checkpoint.

Never mark untested functionality as complete.

8. Final acceptance criteria

Lightning should have:

- A consistent type system.
- Reliable scopes.
- Dynamic collections.
- Structured exception handling.
- A complete practical function system.
- Reliable object operations.
- Consistent operators.
- Useful standard utilities.
- CLI arguments and process interaction.
- Predictable memory management.
- Maintainable compiler architecture.
- Professional diagnostics.
- Extensive regression and adversarial testing.

File parsing and library/package support must remain unimplemented.

However, the architecture must be prepared to support them without requiring another fundamental compiler rewrite.

9. Final report

Produce a detailed report containing:

- Original and final architecture.
- Features implemented.
- Files changed.
- Bugs discovered and fixed.
- Breaking changes.
- Security and memory-safety findings.
- Tests created and executed.
- Passing and failing test counts.
- Fuzzing results.
- Known limitations.
- File-parsing readiness.
- Library-support readiness.

Clearly distinguish verified, partially verified, untested and unresolved functionality.

FINAL COMMAND

Start by auditing the repository.

Then implement all phases sequentially.

You are authorized to make changes within this scope without requesting approval for every phase.

Do not stop after planning, do not rush through phases, and do not declare success merely because the compiler builds.

Think beyond obvious test cases. Challenge your assumptions, investigate unusual interactions, and deliberately attempt to break your own implementations.

The objective is not simply to add features. The objective is to make Lightning reliable enough for real-world programming.
