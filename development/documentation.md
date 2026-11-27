# Lightning (ltng) Documentation

Lightning is a small, statically-parsed, natively-compiled language with dynamic runtime values. Source files use the `.ltng` extension and compile to native executables through the Lightning compiler.

This document describes the current language behavior as implemented in this repository.

## Table of Contents

- [Getting Started](#getting-started)
- [Compilation](#compilation)
- [Syntax Basics](#syntax-basics)
- [Types and Values](#types-and-values)
- [Variables and Constants](#variables-and-constants)
- [Operators and Expressions](#operators-and-expressions)
- [Strings and Interpolation](#strings-and-interpolation)
- [Input and Output](#input-and-output)
- [Control Flow](#control-flow)
- [Functions](#functions)
- [Arrays](#arrays)
- [Objects](#objects)
- [Exception Handling](#exception-handling)
- [Type Conversion and Introspection](#type-conversion-and-introspection)
- [Scoping and Lifetime](#scoping-and-lifetime)
- [Known Limitations](#known-limitations)

## Getting Started

### File extension

Lightning source files use:

```text
program.ltng
```

### Hello World

```ltng
write("Hello, World!")
```

### Compile and run

Use `bolt` (the user-facing interface) or the compiler with `LTNG_BOLT=1` as required by the environment.

Typical compile:

```bash
LTNG_BOLT=1 ltng program.ltng program
./program
```

Useful modes:

```bash
LTNG_BOLT=1 ltng --check program.ltng      # parse/semantic checks only
LTNG_BOLT=1 ltng --emit-c program.ltng out.c  # emit generated C
```

## Compilation

The compiler parses Lightning source, performs semantic/type checks, generates C, and invokes the system C compiler to produce a native executable.

- `--check`: validates without producing a native binary.
- `--emit-c`: writes generated C.
- Normal mode: produces a native executable.

Generated programs are native binaries; runtime behavior is defined by the language semantics implemented by the compiler/runtime.

## Syntax Basics

### Comments

Single-line comments start with `#`:

```ltng
# this is a comment
let x = 1 # inline comment
```

### Statements

Statements are generally written one per line. Blocks use `{ ... }`.

### Blocks

```ltng
if (x > 0) {
    write("positive")
}
```

Blocks create scope for `let`/`const` declarations.

## Types and Values

Lightning has these core types:

| Type | Description | Examples |
|------|-------------|----------|
| `int` | Integer number | `0`, `42`, `-7` |
| `float` | Floating-point number | `3.14`, `-0.5` |
| `string` | Text | `"hello"` |
| `char` | Single character/string-char value | `'a'` (represented as string in many contexts) |
| `bool` | Boolean | `true`, `false` |
| `null` | Null/empty value | `null` (where supported as literal/value context) |
| `array` | Ordered collection | `[1, 2, 3]` |
| `object`/`obj` | Key/value object | object literals / `obj` dtype |

`typeof(value)` returns a type name string such as `"int"`, `"float"`, `"string"`, `"bool"`, `"array"`, `"obj"`, or `"null"` depending on value/context.

## Variables and Constants

### `let`

Declare a variable:

```ltng
let x = 5
let name = "Lightning"
let pi = 3.14
let ok = true
```

#### Dtype-only declaration

```ltng
let n = int
let s = string
let f = float
let b = bool
```

Dtype-only declarations create a default-initialized variable and are locked to that dtype.

#### Value-initialized declaration

Value-initialized `let` declarations are dtype-unlocked by default (their dtype can change through `set`), unless otherwise constrained by semantics.

```ltng
let x = 5
set x = "now a string"   # allowed for unlocked value-initialized let
```

### `const`

Constants cannot be reassigned:

```ltng
const PI = 3.14159
```

### `set`

Use `set` to assign/update variables:

```ltng
let x = 1
set x = 2
set x += 3
```

Compound assignments supported in covered forms include `+=`, `-=`, `*=`, `/=`, `%=`.

### `lock()` and `unlock()`

`lock(x)` prevents dtype changes for a variable; `unlock(x)` allows dtype changes when permitted.

```ltng
let x = 5
lock(x)
# set x = "hi"   # type-locked mismatch rejected
unlock(x)
set x = "hi"
```

## Operators and Expressions

### Arithmetic

| Operator | Meaning |
|----------|---------|
| `+` | Addition / string concatenation (context-dependent) |
| `-` | Subtraction |
| `*` | Multiplication / string repetition in supported contexts |
| `/` | Division |
| `%` | Modulo/remainder |

Notes:

- `int / int` uses integer division semantics.
- Mixed `int`/`float` arithmetic promotes to `float` in covered numeric contexts.
- Division/modulo by zero is rejected at compile time for provable literal cases and throws runtime `DivisionError` / `DivisionError: Modulo by zero` in covered runtime cases.
- Unary numeric operators on string/char literals are rejected at compile time.

### Comparison

`==`, `!=`, `<`, `<=`, `>`, `>=`

Comparisons return boolean-like results (often represented as `true/false` in output and `1/0` in numeric contexts).

### Logical

`and`, `or`, `not` (and `&&`, `||`, `!` where supported)

Logical/comparison conditions are used in `if`, `while`, `for`, ternary, etc.

### Bitwise

Bitwise operators are supported where implemented (`&`, `|`, `^`, `~`, shifts) in supported numeric contexts.

### Ternary

```ltng
let label = (x > 0) ? "positive" : "non-positive"
```

Ternary expressions are supported in covered expression contexts, including nesting.

### Increment/decrement

```ltng
let i = 0
set i++
set ++i
```

Postfix/prefix increment/decrement are supported in covered contexts.

## Strings and Interpolation

### String literals

```ltng
let s = "hello"
```

### Escape sequences

Common escapes include `\n`, `\t`, `\\`, `\"` (as supported by lexer/parser).

### Interpolation

String interpolation is supported with `$[ ... ]`:

```ltng
let name = "World"
write("Hello, $[name]!")
```

`typeof(...)` and expressions can be used inside interpolation in covered cases.

### Concatenation

```ltng
write("Hello, " + "World")
```

`+` can mean numeric addition or string concatenation depending on types/context. Use explicit conversion when intent matters.

## Input and Output

### `write()`

Writes a value to output:

```ltng
write("hello")
write(42)
```

`write()` accepts expressions, variables, function calls, and array/object access in supported forms.

### `read()`

Reads input into a variable:

```ltng
let name = string
read(name)
```

`read()` behavior depends on target type/context and runtime input.

## Control Flow

### `if` / `else if` / `else`

```ltng
if (x > 0) {
    write("positive")
} else if (x < 0) {
    write("negative")
} else {
    write("zero")
}
```

### `while`

```ltng
let i = 0
while (i < 3) {
    write(i)
    set i++
}
```

### `do-while`

```ltng
let i = 0
do {
    write(i)
    set i++
} while (i < 3)
```

### `for`

C-style for:

```ltng
for (let i = 0; i < 3; set i++) {
    write(i)
}
```

For-each over arrays:

```ltng
let arr = [10, 20, 30]
for (x in arr) {
    write(x)
}
```

### `switch`

```ltng
switch (x) {
    case 1 { write("one") }
    case 2 { write("two") }
    default { write("other") }
}
```

### `break`, `continue`, `exit`

- `break`: exits current loop/switch as applicable.
- `continue`: skips to next loop iteration.
- `exit`: exits program (as supported).

## Functions

### Declaration

```ltng
function add(a, b) {
    return a + b
}
```

### Return type annotations

```ltng
function add(a, b) -> int {
    return a + b
}
```

Annotated return types are validated in covered/provable cases.

### Default parameters

```ltng
function greet(name = "world") {
    write("Hello, $[name]")
}
```

### Named arguments

```ltng
function sub(a, b) {
    return a - b
}
write(sub(b = 3, a = 10))
```

### Anonymous functions / closures

```ltng
let inc = function(x) { return x + 1 }
write(inc(5))
```

Closures capture enclosing variables with shared mutable semantics in supported cases.

### Variadic arguments

```ltng
function sum(...nums) {
    let total = 0
    for (n in nums) {
        set total += n
    }
    return total
}
```

### Overloading

Functions may be overloaded by arity in supported Phase 5 behavior.

### UFCS / method-style calls

Lightning supports UFCS-style method calls for compatible functions/methods:

```ltng
let arr = [3, 1, 2]
arr.sort()
```

### Nested functions

Nested function declarations are supported in covered contexts.

## Arrays

### Literals

```ltng
let arr = [1, 2, 3]
```

### Indexing

Arrays are 1-based. Negative indices count from the end. Index `0` is invalid.

```ltng
let arr = [10, 20, 30]
write(arr[1])    # 10
write(arr[-1])   # 30
```

Out-of-bounds behavior in runtime string-based model may return empty string in some contexts; avoid relying on OOB reads.

### Methods

Common array methods:

- `push(value)`
- `pop()` / `pop(index)`
- `insert(index, value)`
- `remove(index)`
- `contains(value)`
- `index_of(value)`
- `sort()`
- `reverse()`
- `copy()`
- `slice(start, end)`
- `length` / `.length`

`copy()` and `slice()` are supported in `let`/`set` and in covered value-consumed expression contexts.

### Iteration

```ltng
for (item in arr) {
    write(item)
}
```

## Objects

Objects support field access/assignment in supported forms.

```ltng
let o = obj
set o.name = "Lightning"
set o.version = 1
write(o.name)
```

Object literals/field access behavior is supported in covered language paths. Array-of-objects and general nested dynamic collections are limited (see limitations).

## Exception Handling

Lightning supports structured exceptions:

```ltng
try {
    throw error("MyError", "something went wrong")
} catch (e) {
    write(e.type)
    write(e.message)
} finally {
    write("cleanup")
}
```

### Forms

- `try { ... } catch (e) { ... }`
- `try { ... } catch { ... }`
- `try { ... } finally { ... }`
- `try { ... } catch (e) { ... } else { ... } finally { ... }`

### Catch object

Catch variable exposes fields such as `type`, `message`, `trace`, `line`, `column`, `function`, `value`, `obj` (as supported).

### Throw

```ltng
throw error("TypeError", "bad value")
throw "simple error"
```

### Semantics

- `finally` runs on normal completion, throw, return, break, and continue (covered semantics).
- `else` runs only when `try` completes normally and requires a `catch`.
- Uncaught exceptions print traceback to stderr and exit with status 1.

## Type Conversion and Introspection

### Conversions

Built-in conversion functions/aliases in covered contexts:

- `toint(x)` / `int(x)`
- `tofloat(x)` / `float(x)`
- `tostring(x)` / `string(x)`
- `tobool(x)` / `bool(x)`
- `tochar(x)` / `char(x)`

```ltng
let n = toint("42")
let s = tostring(3.14)
let b = tobool("true")
```

Invalid literal conversions are rejected at compile time in provable cases. Runtime-invalid conversion behavior depends on the specific conversion/runtime path.

### `typeof()`

```ltng
write(typeof(1))      # int
write(typeof(3.14))   # float
write(typeof("hi"))   # string
write(typeof(true))   # bool
```

## Scoping and Lifetime

- `let` and `const` are block-scoped.
- Inner scopes can shadow outer variables.
- Same-scope duplicate declarations are rejected.
- Function parameters and locals follow function/block scope rules.
- Declared control-flow/standalone blocks create scope.

## Known Limitations

This is a non-exhaustive summary; see `development/limitations.md` for details.

- **Dynamic runtime model:** many runtime values are string-backed/dynamic-heap representations; some semantics are constrained by this model.
- **`+` ambiguity:** `+` can mean addition or concatenation; mixed operands can be context-sensitive.
- **Collections:** no first-class arrays-of-objects/jagged dynamic arrays; multidimensional rectangular arrays are supported.
- **Array OOB reads:** may return empty string in runtime model; don't rely on OOB behavior.
- **Diagnostics/tooling:** diagnostics are improving but not perfect; sanitizer/valgrind availability is environment-dependent.
- **Cross-arch artifacts:** prebuilt multi-arch binaries require the build toolchain (zig/cross compilers) to rebuild/sync.

---

For phase status and verified test counts, see `development/PHASE_STATUS.md`. For known issues, see `development/limitations.md`.