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

`if`, `elseif`, `else`, `for`, `while`, functions, methods, constructors, getters, setters, class bodies, table literals, and destructuring patterns all use braces according to their grammar.

## Chained Expressions

Postfix expression operations can continue across whitespace:

```js
var value = source
    .items
    [0]
    :format("short")
```

The dot operator requires an identifier after it. The colon operator requires an identifier and call arguments after it.

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
