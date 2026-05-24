# Little Syntax

This page documents the source-level syntax rules that are not tied to a specific standard library module.

## Whitespace

Little treats spaces, tabs, carriage returns, and newlines as whitespace. Whitespace separates tokens when needed, but otherwise has no meaning.

These forms are equivalent:

```js
if ready { io.print("yes") }
```

```js
if ready
{
    io.print("yes")
}
```

The same rule applies to call chains, indexes, method calls, argument lists, table literals, array literals, and function bodies:

```js
task.run(async fn(state)
{
    return state.value
},{
    value: "A"
})
.next(fn(value) {
    io.print(value)
})
```

## Comments

A semicolon starts a line comment. The comment runs until the next newline.

```js
; whole-line comment
var score = 10 ; trailing comment
io.print(score)
```

Semicolons are not statement terminators.

## Statements

Statements do not need a terminator token. A statement ends when the parser has a complete expression or declaration and the next token starts another statement, assignment, block close, or enclosing list boundary.

```js
io.print("first")
io.print("second")
```

Adjacent statements on the same physical line are rejected:

```js
io.print("first") io.print("second")
```

Use one statement per line. Semicolons are comments, not statement separators.

## Separators

Commas are accepted in list-like forms, but many lists can also be separated by whitespace alone.

```js
var values = [
    1
    2,
    3
]

var config = {
    name: "little"
    mode: "loose"
}

io.print(
    "hello"
    "world"
)
```

This applies to call arguments, function parameters, array literals, table literals, and destructuring patterns. Use commas when they improve readability or avoid ambiguity, especially in dense call arguments.

## Blocks

Blocks use braces. The opening brace may be on the same line as the header or on a later line.

```js
while running
{
    tick()
}
```

`if`, `elseif`, `else`, `for`, `while`, `with`, functions, methods, constructors, getters, setters, class bodies, table literals, and destructuring patterns all use braces according to their grammar.

## Chained Expressions

Postfix expression operations can continue across whitespace:

```js
var value = source
    .items
    [0]
    :format("short")
```

The dot operator requires an identifier after it. The colon operator requires an identifier and call arguments after it.

## Operators And Grouping

Equality is spelled with words:

```js
if name is "Ada" { ... }
if status isnt "blocked" { ... }
```

Little does not have `==` or `!=`. Use `is` and `isnt`.

Parentheses group expressions and may also wrap control-flow conditions:

```js
var total = (base + bonus) * scale

if (total >= 10) and ready {
    io.print("ready")
}

while (count < max) {
    count = count + 1
}
```

See [lt.md](lt.md) for the full operator precedence and truthiness rules.

## Table Call Sugar

A table literal immediately after a callable expression is treated as a single call argument.

```js
configure {
    name: "little"
    mode: "loose"
}
```

This is equivalent to:

```js
configure({
    name: "little"
    mode: "loose"
})
```

The same sugar works with receiver-passing method calls:

```js
builder:add {
    id: "alpha"
    delay: 10
}
```

This is equivalent to:

```js
builder:add({
    id: "alpha"
    delay: 10
})
```

The table is the explicit argument. For `obj:method { ... }`, the receiver is still passed implicitly as `this`.

Consecutive table-call sugar can call a returned callable:

```js
step1 { name: "a" } { name: "b" } { name: "c" }
```

That parses like:

```js
step1({ name: "a" })({ name: "b" })({ name: "c" })
```

Control-flow headers do not use table-call sugar. In `if ready { ... }`, the brace starts the `if` body.

## Function Declarations

Named functions can be declared without a separate `var` assignment:

```js
fn greet(name) {
    return string.format("hello %s", name)
}
```

This is equivalent to assigning a function literal to a local:

```js
var greet = fn(name) {
    return string.format("hello %s", name)
}
```

Async named functions use the same shape:

```js
async fn fetchName() {
    return await loadName()
}
```

## Globals

Top-level and local declarations are local by default. Use `global` when a script intentionally writes to the VM global table.

```js
global appName = "little"

global fn greet(name) {
    return string.format("hello %s from %s", name, appName)
}

global async fn later(value) {
    return await Promise(fn(resolve, reject) {
        resolve(value)
    })
}
```

`global name = expression` assigns the evaluated expression to the global named `name`. If the initializer is omitted, the global is set to `null`.

There is no implicit global assignment. Plain assignment still requires an existing local or upvalue:

```js
missing = 1 ; error
```

`global var name = value` is not valid syntax. Use `global name = value`.

## Imports

`import "path"` imports the full module value as an expression:

```js
var greeter = import "greeter"
```

`import { ... } from "path"` imports a module and destructures its returned table into locals:

```js
import { greet, shout } from "greeter"
```

See [modules.md](modules.md) for module loading, exporting, path, and cache behavior.

## Receiver Shorthand

Inside class methods, class field initializers, functions whose first parameter is named `this`, and `with` blocks, `@name` is shorthand for `this.name` or the active `with` receiver's `name` field.

```js
class Counter {
    public value = 0

    public add(amount) {
        @value = @value + amount
        return @value
    }
}
```

Receiver-style table functions can use the same shorthand when the first parameter is named `this`:

```js
var counter = {
    value: 0
    add: fn(this, amount) {
        @value = @value + amount
    }
}
counter:add(2)
```

`@` is only shorthand for fields. It does not change bare identifier lookup.

## With Blocks

`with expression { ... }` evaluates the expression once and makes it the active receiver for `@name` inside the block.

```js
with counter {
    @value = @value + 1
    @label = "ready"
}
```

This is equivalent to evaluating `counter` into a hidden local and using that local as the receiver for each `@name` access. Nested `with` blocks temporarily replace the active receiver.

## Constructor Field Parameters

Class constructor parameters may start with `@`. These parameters are automatically assigned to fields with the same name before the constructor body runs.

```js
class Person {
    constructor(@name, @score) {
        @score = @score + 1
    }
}
```

This behaves like:

```js
class Person {
    constructor(name, score) {
        this.name = name
        this.score = score
        this.score = this.score + 1
    }
}
```

## Class Inheritance

Classes may extend one named superclass:

```js
class Dog extends Animal {
    constructor(name) {
        super(name)
    }

    override speak() {
        return string.concat(super.speak(), ":dog")
    }
}
```

`super(...)` is only valid inside constructors. `super.method(...)` is valid inside methods and constructors. Subclass members that share inherited public names must use `override`, and private members remain private to the class that declared them.

## Strings

String literals use double quotes.

```js
var message = "hello"
```

Supported escapes are `\n`, `\r`, `\t`, `\"`, and `\\`. Other escaped characters are preserved with the backslash.

```js
io.print("line one\nline two")
io.print("left\qright")
```

## Numbers

Number literals are decimal numbers or hexadecimal integers.

```js
var a = 123
var b = 0.5
var c = 0xff
var d = 0X10
```

Hex literals are normal numbers.
