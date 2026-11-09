# Lightning (ltng) Documentation

Lightning is a minimal, beginner-friendly programming language. No semicolons, no complex syntax — just write code and go.

**Status:** The language is under active development. This document describes only
features that are currently implemented. A missing feature is not a bug; a
component that exists but behaves incorrectly is.

---

## Getting Started

### File Extension

All Lightning source files use the `.ltng` extension.

### Writing Your First Program

Create a file called `hello.ltng`:

```
write("hello world")
```

### Compiling & Running

**Option 1: Compile and run in one step (via `bolt`)**

```
bolt hello.ltng
```

**Option 2: Compile first, then run**

```
ltng hello.ltng
./hello
```

**With debug output**

```
bolt --debug hello.ltng
```

---

## Syntax

### Comments

Use `#` to write comments. Comments run to the end of the line and are ignored by the compiler.

```
# this is a comment
write("hello") # inline comment
```

### Statements

Each statement goes on its own line. **Semicolons are not allowed** — the compiler rejects them.

```
write("hello")
write("world")
```

One statement per line is the convention.

---

## Output

### Writing Text

Use `write()` to print text to the screen. A newline is added automatically.

```
write("hello world")
write("lightning is fast")
```

### Escape Sequences

| Sequence | Meaning |
|----------|---------|
| `\n`     | Newline |
| `\t`     | Tab     |
| `\\`     | Backslash |
| `\"`     | Double quote |
| `\'`     | Single quote |
| `\0`     | Null character |

```
write("line one\nline two")
write("tab\there")
```

### String Interpolation

Use `$[variable]` inside strings to embed variable values:

```
let name = "bolt"
let age = 10
write("Hello, $[name]!")
write("Name: $[name], Age: $[age]")
```

Output:

```
Hello, bolt!
Name: bolt, Age: 10
```

Expressions work inside interpolation too:

```
write("Result: $[x + 1]")
write("Type: $[typeof(x)]")
```

Object fields work inside interpolation:

```
let x = { name = "saim", age = 17 }
write("Hello $[x.name]")     # Hello saim
write("Age $[x.age]")        # Age 17
```

A literal `$` not followed by `[` is preserved as-is. Use `<...>` to display literal characters inside strings:

```
write("cost is $5")          # cost is $5
write("<$[x]>")              # $[x]
write("<">")                 # "
write("<<")                  # <
write("<<>")                 # <
write("$[x] and <$[x]>")     # 5 and $[x]
```

### Angle Brackets

Text between `<` and `>` inside a string is treated literally (brackets are stripped by the compiler):

```
write("<raw>")        # raw
write("<b>bold</b>")  # bold
write("2 < 3")        # 2 < 3
write("a<b")          # ab
write("<<")           # <
write("<<>")          # <
```

---

## Input

### Reading User Input

Use `read()` to get input from the user. The variable must already be declared.

```
let name = string
let age = int
let score = int
let letter = char
let flag = bool

write("Enter your name:")
read(name)

write("Enter your age:")
read(age)

write("Enter a score:")
read(score)

write("Enter a letter:")
read(letter)

write("Enter true/false:")
read(flag)
```

You can also read into a null variable declared with bare `let x`. The `read()`
does **not** lock the null variable's type — it just stores the input value.
This means you can `read()` into the same null variable multiple times:

```
let x          # null variable
read(x)        # x stores the input, type stays null
write(x)       # prints whatever was typed
read(x)        # read again - type still null, new value stored
set x = 42     # OK - null allows one type transition
```

### Read Behavior by Type

| Type | Reads | Example Input |
|------|-------|---------------|
| `int` | Integer (validated — non-numeric input causes runtime error) | `42`, `-10` |
| `char` | Single character (first char of input) | `A`, `z` |
| `string` | Raw input line | `hello` |
| `bool` | `true`/`t` → true, anything else → false | `true`, `false` |
| `obj` | Not supported (compile-time error) | — |

Input is read as a full line (until newline). For `int` variables, the input is
validated — if the user enters a non-numeric string, the program exits with a
runtime error. Numeric input is converted (`atoi`) and stored back as a string
buffer.

---

## Variables

### Declaration with Type Inference

Use `let` to declare a variable. The type is inferred automatically from the value.

```
let x = 42
let name = "lightning"
let pi = 3.14
let letter = 'A'
```

### Bare `let` (null variable)

Use `let x` without an assignment to declare a `null`-typed variable that can
hold any type. The first `set x = <value>` locks the type; after that the type
cannot change (dtype immutability). Bare `let x` allows one type transition on
first `set`.

```
let x           # null variable
write(x)        # prints empty line
set x = 5       # x becomes int
write(x)        # 5
set x = 10      # OK, same type (int)
set x = "hi"    # error: cannot change type from int to string
```

### Explicit Type Declaration

You can specify the type explicitly by placing a type keyword after `=` (without
a value). The variable is initialized to the type's default.

```
let x = int        # x = 0
let score = float  # y = 0.0
let greeting = string  # s = ""
let letter = char  # c = '\0'
```

A typed declaration cannot include a value on the same line:

```
let x = int 42     # error: cannot assign a value with a type declaration
let score = float 99.5  # error
```

Use a separate `set` to assign a value after declaring the type:

```
let x = int
set x = 42   # OK
```

### Constants

Use `const` to declare a constant. Constants must be initialized with a value
and cannot be changed afterward. The value can be any dtype (int, float, string,
char, bool) — type declarations like `const x = int` are **not** supported.

```
const pi = 3.14
const name = "lightning"
const max = 100
const flag = true
const letter = 'A'
```

Uninitialized constants are not allowed:

```
const x = int     # error: const requires a value assignment, not a type declaration
const x = int 5   # error: const requires a value assignment, not a type declaration
```

### Setting Variables

Use `set` to change the value of a variable. After a variable's type is locked
(e.g. by `let x = 5` or `let x = int`), `set` cannot change the type.

```
let x = int        # OK, x = 0 (default)
set x = 20          # x is now 20 (int)
set x = float 3.14  # error: cannot change type from int to float
set x = "hello"     # error: cannot change type from int to string
```

The variable must already be declared:

```
set y = 10          # error: Variable not declared
```

You cannot assign to a constant:

```
const c = 1
set c = 2           # error: Cannot assign to constant
```

### Object literals and the `obj` dtype

Objects group named fields into a single value. Create an object literal with
`{ ... }` and `field = value` pairs (values are literals: string, int, float,
bool, or char).

```
let x = {
    default = "object"
    name = "saim"
    age = 17
    class = "xi"
}

write(x)        # prints the "default" field value -> object
write(x.age)    # field access -> 17
write(x.name)   # -> saim
write(x.obj)    # all field values in order -> object saim 17 xi
```

The `default` field defines what `write(x)` displays. `obj` is a synthesized
view of every value in field order.

You can also declare an object-typed variable explicitly:

```
let x = obj                  # x is an object-typed null variable
set x = { name = "saim", age = 17 }   # assign an object literal
write(x.age)                 # 17
```

Object fields can be used inside string interpolation:

```
let x = { default = "myobj" name = "saim" age = 17 }
write("the name is $[x.name]")   # the name is saim
write("age is $[x.age]")         # age is 17
```

Object fields can also be accessed as function parameters — the schema transfers
to the parameter name:

```
function show(p) {
    write(p.age)
    write(p.default)
}

let person = obj
set person = { default = "Person" name = "Saim" age = 17 }
show(person)   # prints 17, Person
```

### Booleans

Use `true`/`t` for true and `false`/`f` for false:

```
let flag = true
let off = false
let flag2 = bool true
let off2 = bool false

write(flag)    # true
write(off)     # false
```

### Type Introspection with typeof()

Use `typeof()` to get the type of a variable or literal value as a string. This is resolved at compile time.

```
let x = 10
let t = typeof(x)
write(t)        # int

write(typeof(3.14))    # float
write(typeof("hi"))    # string
write(typeof('A'))     # char
write(typeof(true))    # bool
```

`typeof()` accepts variables, literal values, reserved words, and nested typeof calls, returning:

| Input | Returns |
|-------|---------|
| `typeof(3)` | `"int"` |
| `typeof(3.14)` | `"float"` |
| `typeof("hello")` | `"string"` |
| `typeof('A')` | `"string"` |
| `typeof(true)` | `"bool"` |
| `typeof(int)` | `"reserved"` |
| `typeof(let)` | `"reserved"` |
| `typeof(x)` | looks up variable type |
| `typeof(typeof(x))` | `"string"` |

It works with let, const, set, and write:

```
const my_pi = 3.14
const my_type = typeof(my_pi)    # "float"

let v = int 10
set v = typeof(3.14)             # v becomes the string "float"
write(typeof(v))                 # string
```

### Type Keywords

| Keyword | Type | Example Value |
|---------|------|---------------|
| `int` | Integer (includes float values) | `42`, `0`, `-10`, `3.14` |
| `string` | Text (includes char values) | `"hello"`, `'A'`, `"world"` |
| `bool` | Boolean | `true`, `false` |
| `obj` | Object | `{ name = "saim", age = 17 }` |

The `char` and `float` keywords have been removed. All numeric values are treated as `int`,
and all character values are treated as `string`. A `char` literal like `'A'` is a valid
string value and can be assigned to `string` variables.

The `float` keyword has been removed. All numeric values (integers and floats)
are treated as `int` type. A variable declared with `let x = int` can be assigned
any numeric value via `set x = 3.14`, and `typeof()` always returns `"int"` for
numbers.

### String Concatenation

Strings can be concatenated using the `+` operator:

```
let s = "hello" + "world"
write(s)
write("hello " + "world")
```

### Type Inference Rules

Without explicit type, the compiler detects the type automatically:

| Value | Detected Type |
|-------|---------------|
| `42` | int |
| `3.14` | int |
| `"hello"` | string |
| `'A'` | string |
| `true` | bool |

### Using Variables

Pass a variable name to `write()` to print its value.

```
let age = 25
let name = "bolt"
let score = 99.5
let grade = 'A'

write(age)
write(name)
write(score)
write(grade)
```

Output:

```
25
bolt
3.14
A
```

---

## Objects

Objects group named fields into a single value. Create an object literal with `{ ... }` and `field = value` pairs (values are literals: string, int, float, bool, or char).

```
let x = {
    default = "object"
    name = "saim"
    age = 17
    class = "xi"
}
```

- `write(x)` prints the object's `default` field value.
- `write(x.age)` / `write(x.name)` accesses a field by name.
- `write(x.obj)` prints all field values in order (space-separated).

```
write(x)        # object
write(x.age)    # 17
write(x.obj)    # object saim 17 xi
```

The `default` field defines what `write(x)` displays. `obj` is a synthesized view of every value in field order.

Empty objects are allowed:

```
let e = {}
write(e)        # object
write(e.obj)    # (empty)
```

---

## Expressions & Operators

Expressions can be used anywhere a value is expected: in `let`, `const`, `set`, `write()`, and inside string interpolation `$[...]`.

```
let x = 2 + 3 * 4
set x = (x + 1) * 2
write(x)
write("result: $[x / 2]")
```

### Arithmetic Operators

| Operator | Meaning | Example | Result |
|----------|---------|---------|--------|
| `+` | Addition | `2 + 3` | `5` |
| `-` | Subtraction | `10 - 4` | `6` |
| `*` | Multiplication | `3 * 7` | `21` |
| `/` | Division (integer truncates) | `10 / 3` | `3` |
| `/` | Division (float) | `10.0 / 3.0` | `3.33333` |
| `%` | Modulo | `10 % 3` | `1` |
| `-` | Unary minus | `-5`, `-x` | negated value |

Integer division truncates: `7 / 2` gives `3`. If either operand is a float,
the result is a float.

### Comparison Operators

Comparisons evaluate to an integer: `1` for true, `0` for false.

| Operator | Meaning | Example | Result |
|----------|---------|---------|--------|
| `==` | Equal | `5 == 5` | `1` |
| `!=` | Not equal | `5 != 3` | `1` |
| `<` | Less than | `3 < 5` | `1` |
| `>` | Greater than | `5 > 3` | `1` |
| `<=` | Less than or equal | `5 <= 5` | `1` |
| `>=` | Greater than or equal | `4 >= 5` | `0` |

Comparisons work on numbers, characters, and booleans. String comparison is not
supported — `==` on two strings compares addresses, not content. Use a simple
variable check (`if (text)`) or compare against a flag instead.

### Logical Operators

Boolean literals print as `true`/`false`, but logical and comparison expressions evaluate to integers (`1`/`0`).

| Operator | Meaning | Example | Result |
|----------|---------|---------|--------|
| `&&` | And | `true && false` | `0` |
| `\|\|` | Or | `true \|\| false` | `1` |
| `!` | Not | `!true` | `0` |
| `^^` | XOR | `true ^^ true` | `0` |

The words `and`, `or`, and `not` can be used in place of `&&`, `||`, and `!`:

```
write(true and false)  # 0
write(true or false)   # 1
write(not true)        # 0
```

### Bitwise Operators

When both operands are integers, `&&`, `||`, `!`, and `^^` operate on the bits instead:

| Operator | Meaning | Example | Result |
|----------|---------|---------|--------|
| `&&` | Bitwise AND | `15 && 9` | `9` |
| `\|\|` | Bitwise OR | `15 \|\| 9` | `15` |
| `!` | Bitwise NOT | `!0` | `-1` |
| `^^` | Bitwise XOR | `15 ^^ 9` | `6` |

```
write(true && false)   # 0 (logical)
write(15 && 9)         # 9 (bitwise)
```

### Precedence

Operators bind from highest to lowest:

1. `()` — grouping
2. unary `-`, `!`
3. `*`, `/`, `%`
4. `+`, `-`
5. `<`, `>`, `<=`, `>=`
6. `==`, `!=`
7. `&&` (bitwise `&` on ints)
8. `^^` (`^`)
9. `||` (bitwise `|` on ints)

Parentheses override precedence:

```
write(2 + 3 * 4)     # 14
write((2 + 3) * 4)   # 20
```

### Booleans and Chars in Arithmetic

Boolean and char variables can be used in numeric expressions. A bool is treated as `1`/`0`, and a char is treated as its character code.

```
let flag = true
let letter = 'A'
write(flag + 1)      # 2
write(letter + 1)    # 66
```

### String Multiplication

Use `*` to repeat a string a specified number of times.

```
let stars = "*" * 5
write(stars)  # *****

let repeated = "ab" * 3
write(repeated)  # ababab

let msg = 3 * "ha"
write(msg)  # hahaha
```

String multiplication has higher precedence than string concatenation.

String concatenation works in assignments, `write()` statements, `if` conditions, and `switch` statements.

Format specifiers (like `%s`, `%d`, `%f`, etc.) are **not allowed** in string literals. Use string concatenation, string interpolation with `$[variable]`, or other string operations instead.

---

## Increment and Decrement

Use `++` and `--` for pre-increment/decrement and post-increment/decrement. They
can be used as statements (on their own line) or as expressions inside `write()`,
`if`, `switch`, and other expression contexts.

```
let i = 5
++i        # pre-increment (statement)
i++        # post-increment (statement)
--i        # pre-decrement (statement)
i--        # post-decrement (statement)
write(i)

# As expressions inside write():
write(++x)   # returns the new value
write(x++)   # returns the old value
write(--x)   # returns the new value
write(x--)   # returns the old value
```

`++`/`--` work on `int`, `float`, `string`, `char`, and `bool` variables.

### Compound Assignment Operators

Use compound assignment operators to modify a variable in place.

```
let x = 10
set x += 5   # x = x + 5
set x -= 3   # x = x - 3
set x *= 2   # x = x * 2
set x /= 4   # x = x / 4
set x %= 2   # x = x % 2
write(x)
```

---

## Conditionals

Use `if` to run a block of code only when a condition is true. The condition goes inside parentheses `( )`, and the block is wrapped in `{ }`. Each statement inside goes on its own line.

```
let age = 20
if (age >= 18) {
    write("adult")
}
```

### else

```
let age = 15
if (age >= 18) {
    write("adult")
} else {
    write("minor")
}
```

### else if

Chain multiple conditions with `else if`:

```
let score = 85
if (score >= 90) {
    write("A")
} else if (score >= 80) {
    write("B")
} else if (score >= 70) {
    write("C")
} else {
    write("F")
}
```

### Conditions

A condition may be a comparison, a logical expression, a boolean variable, or a number. Any nonzero number and `true` are treated as true; `0` and `false` are false.

```
let n = 3
if (n) {
    write("nonzero is true")
}

let flag = true
if (flag and n > 2) {
    write("both true")
}
```

### Nesting

```
if (n > 0) {
    if (n % 2 == 0) {
        write("positive even")
    } else {
        write("positive odd")
    }
}
```

### Variables and Blocks

`let` and `const` declarations are **not allowed** inside `if`/`else` blocks, `switch`/`case`/`default` blocks, or loop bodies (`for`, `while`, `do-while`). Declare variables before the control structure and use `set` to modify them inside the block.

```
let x = 0
if (true) {
    set x = 5   # OK
    # let y = 1 # error: 'let' not allowed inside control blocks
}
```

---

## Switch Statement

Use `switch` to compare a value against multiple cases. The condition goes in parentheses `()` and cases are wrapped in `{ }`. Every `case` and `default` must be followed by `{ }`.

```ltng
let x = 2
switch (x) {
    case 1: {
        write("one")
    }
    case 2: {
        write("two")
    }
    default: {
        write("other")
    }
}
```

### Full expressions in cases

Cases can contain full expressions:

```ltng
let x = 3
let y = 2
switch (x) {
    case y > 1: {
        write("yes")
    }
    default: {
        write("no")
    }
}
```

### Multiple cases and different types

```ltng
let c = 'A'
switch (c) {
    case 'A':
    case 'a': {
        write("letter A")
    }
    default: {
        write("other")
    }
}

let flag = true
switch (flag) {
    case true: {
        write("true")
    }
    case false: {
        write("false")
    }
}
```

### Rules

- Switch works with `int`, `char`, and `bool` values
- `case` values must be constants (literals)
- `default` is optional and matches if no case matches
- Multiple `default` cases are not allowed

---

## Loops

### For Loop

Use `for` with `from ... to ...` to iterate over a range.

```
for (i from 0 to 5) {
    write(i)
}
```

Add `step` to control how the loop variable changes each iteration. A plain number means an additive step (e.g., `step 2` is the same as `step +2`). Explicit operators `+`, `-`, `*`, and `/` are supported; `%` is not allowed in `step`.

```
for (i from 0 to 10 step 2) {
    write(i)
}

for (i from 5 to 0 step -1) {
    write(i)
}

for (i from 1 to 16 step *2) {
    write(i)
}

for (i from 64 to 1 step /2) {
    write(i)
}
```

### While Loop

Use `while` to repeatedly execute a block of code as long as a condition is true.

```
let i = 0
while (i < 5) {
    write(i)
    set i = i + 1
}
```

### Do-While Loop

Use `do-while` to execute a block of code at least once, then repeatedly while the condition is true.

```
let i = 0
do {
    write(i)
    set i = i + 1
} while (i < 3)
```

The do-while loop always executes the body at least once, even if the condition is false initially.

### Break

Use `break` to exit from a loop early.

```
let i = 0
while (i < 10) {
    if (i == 5) {
        break
    }
    write(i)
    set i = i + 1
}
```

### Continue

Use `continue` to skip the rest of the current loop iteration and move to the next one. `continue` is only valid inside loops (`for`, `while`, `do-while`), not inside `switch` cases.

```
let i = 0
while (i < 5) {
    set i = i + 1
    if (i == 3) { continue }
    write(i)
}
# prints: 1 2 4 5
```

---

## Functions

Define a named function with `function`, a name, parameters, and a body block.

```
function add(a, b) {
    return a + b
}

let result = add(2, 3)
write(result)        # 5
write(add(10, 5))    # 15
```

Functions can take any number of parameters (including none) and can `return` a value. A `return` exits the function immediately.

```
function greet(name) {
    return name
}

write(greet("hello"))   # hello

function sayhi() {
    write("hi")
    return 0
}
sayhi()
```

### Locals and scoping

Use `let` and `const` inside function bodies to declare local variables. As with other code, `let`/`const` declarations are **not allowed** inside `if`/`else`, `switch`/`case`/`default`, or loop bodies (including when those appear inside a function) — declare variables before those control blocks.

```
function compute(x) {
    let doubled = x + x
    const bonus = 1
    if (doubled > 5) {
        return doubled + bonus
    } else {
        return doubled
    }
}
```

### Calls

Functions can be called as statements, in `let`/`const`/`set` assignments, in `write()`, and inside expressions and conditions. Functions can call other functions and recurse. Top-level functions support forward references (a function can call another top-level function defined later in the file).

```
function fact(n) {
    if (n <= 1) {
        return 1
    } else {
        return n * fact(n - 1)
    }
}
write(fact(5))   # 120
```

### Method-style calls (UFCS)

A function can also be called with method-style syntax: `receiver.func(args...)`. The receiver becomes the first argument, so `receiver.func(args...)` is equivalent to `func(receiver, args...)`. This works with any first parameter name (for example `base`).

```
function a(base) {
    return base + 1
}

let x = 5
write(x.a())        # 6  (same as a(x))

function add(base, n) {
    return base + n
}
write(x.add(10))    # 15 (same as add(x, 10))
let y = x.add(3)    # assignment
set x = x.add(1)    # set
```

### Nested functions

A `function` can be defined inside another function's body. Nested functions are scoped to the enclosing function (the name is visible within that enclosing function) and are lifted internally with a unique name, so the same nested name can be used in different functions without conflict.

```
function outer(a) {
    function inner(b) {
        return b * 2
    }
    return inner(a) + 1
}
write(outer(5))   # 11

function one() { function helper() { return 1 }  return helper() }
function two() { function helper() { return 2 }  return helper() }
write(one())  # 1
write(two())  # 2
```

- Nested function declarations follow the same placement rule as `let`/`const`: they are **not allowed** inside `if`/`else`, `switch`/`case`/`default`, or loop bodies (allowed in function bodies).
- A nested function must be **defined before it is called** within its enclosing scope (nested functions do not have local forward-reference pre-scanning).
- Nested functions are **non-capturing**: they can use their own parameters/locals, but they cannot access the enclosing function's locals or parameters.

---

## CLI Reference

### bolt — Main Command

```
bolt [--debug] [--check] [--compile] [--install] [--uninstall] [--reinstall] <file.ltng> [args...]
```

| Flag | Description |
|------|-------------|
| `--check` | Check syntax only (no compilation) |
| `--compile` | Compile to `compiled/<name>/` with per-architecture binaries and a universal executable |
| `--install` | Install ltng and bolt to /usr/local/bin/ |
| `--uninstall` | Remove ltng and bolt from /usr/local/bin/ |
| `--reinstall` | Uninstall then install |
| `--debug` | Show compilation log |
| `--help` | Show help message |

**Examples:**

```
bolt hello.ltng            # compile and run (no executables left behind)
bolt --compile hello.ltng  # compile only
bolt --debug hello.ltng    # with debug output
bolt --install             # install to /usr/local/bin/
bolt --uninstall           # remove from /usr/local/bin/
bolt --reinstall           # uninstall then install
```

### ltng — Compiler

Compile a `.ltng` file to a native executable.

```
ltng [--debug] [--check] [--emit-c] [--cc <compiler>] <file.ltng> [output]
```

| Flag | Description |
|------|-------------|
| `--debug` | Show compilation log |
| `--check` | Only check for errors, don't compile |
| `--emit-c` | Emit C source instead of compiling |
| `--cc <compiler>` | Use a specific C compiler |
| `<output>` | Output executable path |

---

## Rules

- No semicolons — the compiler will reject them
- One statement per line
- Variable names must start with a letter or underscore
- Variable names can only contain letters, numbers, and underscores
- Strings must be wrapped in double quotes
- Chars must be wrapped in single quotes and contain exactly one character
- Comments start with `#`
- Blocks are wrapped in `{ }`, one `{` per block and one `}` to close it
- `let` and `const` declarations are not allowed inside `if` statements, `switch` statements, or loops (`for`, `while`, `do-while`)
- `let x = int` declares without a value (default 0); `let x = int 42` is **not** allowed — use `set x = 42` afterward
- `const x = 42` requires a value; `const x = int` or `const x = int 42` are **not** allowed
- After a variable's type is locked, `set` cannot change it (dtype immutability). Bare `let x` (null) allows one type transition on first `set`.
- Reserved words cannot be used as variable names: `let`, `const`, `set`, `write`, `read`, `typeof`, `if`, `else`, `switch`, `case`, `default`, `break`, `continue`, `while`, `do`, `for`, `from`, `to`, `step`, `and`, `or`, `not`, the type keywords, and `true`/`false`
- C reserved words (e.g. `return`, `break`, `continue`, `switch`, `case`, `default`, `struct`, `main`, `printf`) **can** be used as variable names — they are mangled internally with a `_res` suffix.

---

## Current Limitations

The following are **not yet implemented** and are not considered bugs:

- File I/O built-ins beyond `read()` from stdin
- Arrays / lists / collections
- Modules / imports
- String comparison by content
- Fallthrough in `switch` cases
- Default function parameter values
- `else if` without braces after `switch` cases (every `case`/`default` needs `{ }`)
