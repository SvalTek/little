# little - language overview
## Types
Little supports a set of basic types:
* `null` - represents the absense of a value
* `number` - a double-precision floating point number
* `boolean` - either true or false
* `string` - a reference to an immutable string
* `function` - a little-defined function
* `closure` - any function that captures surrounding values
* `array` - 0-indexed array of values
* `table` - a table of key-value pairs
* `promise` - an asynchronous value that can be fulfilled or rejected
* `native` - reference to a natively defined C function
* `ptr` - userdata pointer set by C api

These are grouped into `Value` and `Object` types, which are passed by value and reference respectively
`null`, `number`, `boolean`, and `string` are the `Value` types. String is special in that it is immutable and stored in a global deduplication table, and the actual value passed around is an index into that.

---

## Language statements
For layout, comments, separators, and expression boundary rules, see [syntax.md](syntax.md).

### var
```js
var a
var b = 10
var c = (a or 10) + b
var [first, second] = values
var { id, name } = user
var { id: userId } = user
```
Variables are declared with the `var` keyword. A declaration may bind a single name, or destructure values from an array or table.

Array destructuring reads 0-based positions and binds missing slots as `null`:
```js
var [a, b, c] = [ 10, 20 ]
```

Table destructuring supports shorthand keys and rename forms. Missing keys bind as `null`:
```js
var { id, name } = user
var { id: userId, name: displayName } = user
```

Destructuring is declaration-only in v1. It is not supported in assignment targets, parameters, nested patterns, defaults, or rest patterns.

---
### if
```js
if a is 100 { ... }
elseif a is 150 { ... }
elseif b is a { ... }
else { ... }
```
Branching is done with the `if` statement, followed by an expression to evaluate and then a mandatory set of braces containing the body to execute. `if` conditions do not require parentheses:

```js
if score >= 10 and status isnt "blocked" {
    io.print("ready")
}
elseif not enabled {
    io.print("disabled")
}
else {
    io.print("waiting")
}
```

`if` can be followed by any number of `elseif` statements, and optionally a final `else` statement. Each `if` and `elseif` condition is an ordinary expression. `null` and `false` are falsy; every other value is truthy.

---
### for
```js
var a = [ 100, 200, 300 ]
for item in array.each(a) { ... }
```
`for` loops require one loop-variable identifier and an expression that evaluates to an iterator function. Little repeatedly calls that function, stores its return value in the loop variable, and stops when it returns `null`.

Arrays are not iterators themselves. Wrap an array with `array.each(...)`:

```js
for item in array.each(items) {
    io.print(item)
}
```

Passing an array directly to `for` attempts to call the array and raises a runtime error.

---
### while
```js
var a = 0
while a < 10 { a = a + 1 }
```
`while` loops continually evaluate an ordinary expression condition and execute their bodies while it is truthy. Parentheses are not required around the condition; braces are required around the body:

```js
while remaining > 0 and not stopped {
    remaining = remaining - 1
}
```

---
### break
```js
while true { break }
```
`break` exits a loop early.

---
### return
```js
return "any expression!"
return unpack(values)
```
`return` exits the current execution frame. It normally returns a single value to the caller. `return unpack(array)` returns multiple values.

---
### assignment
```js
var a = 10
a = 20
user.name = "Ada"
items[0] = "first"
```
Assignment uses a single `=`. Little does not have compound assignment operators such as `+=` or `-=`.

Valid assignment targets are:

* an existing local or upvalue name: `a = 20`
* a table, array, or instance index: `items[0] = value`
* dot sugar for an index: `user.name = "Ada"`

Assigning to an undeclared bare name is an error. Use `var name = value` for a local declaration or `global name = value` for an intentional VM-global write.

---
Any top-level statement that doesn't match any of these is instead executed as an `expression`

---
## Language expressions
Expressions consist of all literals and operators.

---
### Literals
* `null` is both a type and a literal value
* `number` literals are decimal number strings like `123`, `0.5`, and `123.123`, or hexadecimal integer strings like `0xff` and `0X10`. Hex literals are normal numbers, not strings or byte arrays.
* `boolean` literals are either `true` or `false`
* `string` literals are any double-quoted strings - `"hello world!"`, `"i love apples"`. Supported escapes are `\n`, `\r`, `\t`, `\"`, and `\\`.
* `array` literals are a list of values between brackets - `[ 1, true, null, "banana" ]`
* `table` literals are `key: value` pairs grouped between braces - `{ a: 10 b: 20 c: true }`
    * Identifier shorthand is supported: `{ id, name }` expands to `{ id: id name: name }`
    * Keywords can be used as keys in `key: value` form - `{ if: 1 var: 2 }`. Keyword shorthand is not supported: `{ if }` is an error.
    * Literal keys (`null`, `true`, `false`, `number`, `string`) are valid in `key: value` form - `{ "name": "Ada" 1: "one" }`. Operator tokens like `and` cannot be used as keys.
* `function` literals are declared with this syntax: `var my_fn = fn(a, b) { return a + b }`
    * They are first-class objects, and can only be stored through assignment
    * Can be trivially passed as parameters as well
    * Parameter list is mandatory, even if empty
* Named functions can be declared with `fn name(a, b) { ... }`, which binds a local named `name`
* Named async functions can be declared with `async fn name(a, b) { ... }`
* `async fn` literals return a promise when called, and may use `await`
* `class` declarations create callable class objects that construct instances
### Operators
The mathematical operators `+`, `-`, `*`, and `/` only operate on `number` values.
The comparison operators `<`, `<=`, `>`, and `>=` also only work with `number`s.
The equality operators `is` and `isnt` work on all types. Little does not have `==` or `!=`; use `is` and `isnt`.
The logical operators `or`, `and`, and `not` compare values based on truthiness. `not` returns a boolean. `and` returns a boolean. `or` returns the left operand when it is truthy, otherwise the right operand when it is truthy, otherwise `false`.
`type` is a prefix operator that returns a type-name string without call parentheses. It returns `null`, `boolean`, `number`, `string`, `function`, `table`, `array`, `promise`, `class`, `instance`, or `pointer` for user-visible values.

`typeof` returns an instance's concrete class object. Applied to a class, it returns that class itself; for every other value it returns `null`.
The index operator `[expression]` works on any `table` and `array` values.
The dot operator `.` is syntax sugar for indexing `tables` - `my_table.my_index = 10`. Keyword names can be used after the dot: `t.if` reads the key `"if"`.
The colon operator `:` is syntax sugar for receiver-passing method calls - `obj:method(a)` is equivalent to `obj.method(obj, a)`. Methods conventionally name the first parameter `this`. Keyword method names work too: `obj:if(a)` calls the key `"if"` with `obj` as receiver. The `@` shorthand (`@name`) also accepts keywords: `@if` is `this.if`. Word operators (`and`, `or`, `not`, `is`, `isnt`) are not valid member access names after `.`, `:`, or `@`.

Important: `and` is not a value-selection operator in Little. It always returns a boolean. Use `if` for guarded access or guarded calls:

```js
var label = null

if user {
    label = user.name
}
```

Do not write Lua-style guards such as `user and user.name` when you need `user.name`; that expression returns `true` or `false`, not the selected value. `or` can be used for simple defaults:

```js
var label = requestedLabel or "untitled"
```

Operator precedence, from highest to lowest:

1. Calls, indexing, dot access, and receiver calls: `fn()`, `value[key]`, `table.key`, `obj:method()`
2. Unary operators: `not value`, `-value`, `type value`, `typeof value`
3. Multiplication and division: `*`, `/`
4. Addition and subtraction: `+`, `-`
5. Comparisons and equality: `<`, `<=`, `>`, `>=`, `is`, `isnt`
6. Logical operators: `and`, `or`

Binary operators associate left-to-right within the same precedence level. Prefix unary operators associate right-to-left, so `not not false` works as `not (not false)`.

`type` applies to the following expression, so it reads naturally in a condition:

```js
if type value is "string" {
    io.print(value)
}
```

`typeof` returns the concrete class object, which can be compared with the
existing identity operator:

```js
io.print(typeof apple) ; Apple

if typeof apple is Apple {
    io.print("an Apple")
}
```


This means:

```js
1 + 2 * 3 is 7        ; true, because * runs before +
not false is true     ; true, because not runs before is
1 is 1 and false      ; false, because is runs before and
false or "fallback"   ; "fallback"
true and "value"      ; true
10 - 3 - 2            ; 5, because binary operators group left-to-right
```

Use parentheses to group an expression when the default precedence is not what you want:

```js
(1 + 2) * 3           ; 9
if (score + bonus) >= 10 { ... }
while (count < max) and running { ... }
```

Because `and` and `or` have the same precedence, prefer simple expressions or split complex conditions into named locals when mixing them heavily.

### Function and method calls

Normal calls evaluate the callee expression, then the argument expressions:

```js
fn add(a, b) {
    return a + b
}

io.print(add(2, 3))
```

Functions are lexically scoped and can close over surrounding locals:

```js
fn makeAdder(amount) {
    return fn(value) {
        return value + amount
    }
}
```

The colon form passes the receiver as the first argument:

```js
var counter = {
    value: 0
    add: fn(this, amount) {
        @value = @value + amount
        return @value
    }
}

counter:add(2) ; same as counter.add(counter, 2)
```

Inside a function whose first parameter is named `this`, `@name` is shorthand for `this.name`.

#### Keyword-named receiver fields

The `@name` receiver shorthand also accepts reserved words as field names.

For example:

```little
if @else {
    return @return
}
```

This is valid Little (inside any context where `this` is available, such as a function whose first parameter is named `this`, a class method, or a `with` block).

> Historical note:
> This only exists because a coding model accidentally introduced the insanity,
> and somehow... I kind of like the fact that it's possible.

Table-call sugar lets a table literal immediately after a callable become one argument:

```js
configure {
    name: "little"
    debug: true
}
```

See [syntax.md](syntax.md) for details on table-call sugar and expression boundaries.

### Multiple returns
Little supports multiple return values through `unpack(array)`. In scalar contexts, only the first returned value is used, or `null` when no values are returned.

Final call arguments expand:
```js
fn(1, unpack([ 2, 3 ])) ; fn(1, 2, 3)
```

Non-final call arguments use only the first value:
```js
fn(unpack([ 1, 2 ]), 3) ; fn(1, 3)
```

Destructuring declarations consume multiple returns and pad missing values with `null`:
```js
var [a, b, c] = unpack([ 1, 2 ])
```

### Imports
`import { name } from "module"` loads a Little module and destructures the table it returns into local variables. `import "module"` imports the full returned module value as an expression.

```js
import { greet, shout } from "greeter"

io.print(greet("Ada"))
```

`from` is a contextual identifier: it is only special as the separator between the destructuring pattern and the module path in named-import syntax. Anywhere else, `from` is an ordinary identifier and can be used as a variable name or table key:

```js
var from = "a normal identifier"
import { greet } from "greeter"
```

Modules export values by returning them, commonly as a table. See [modules.md](modules.md) for module loading, exporting, path, and cache behavior.

### Globals
Declarations are local by default. `global name = expression` writes to the VM global table, and global functions can be declared with `global fn name(...) { ... }` or `global async fn name(...) { ... }`:
```js
global answer = 42

global fn readAnswer() {
    return answer
}
```

There are no implicit global writes. Assigning to an undeclared identifier is still an error. Use [syntax.md](syntax.md) for the complete global syntax rules.

### Truthiness
Any `null` or `false` values are considered `falsy`, anything else is logically `true`

---
## Classes
Classes are declaration-style object templates with constructors, public members, private members, and implicit `this` inside class methods:
```js
class Counter {
    private value = 0
    public label = "counter"
    private doubled = this.value * 2

    constructor(start) {
        this.value = start
        this.label = "ready"
    }

    public add(amount) {
        this.value = this.value + amount
        return this.value
    }

    current() {
        return this.value
    }
}

var counter = Counter(2)
io.print(counter.label)
io.print(counter:add(3))
```

`public` is optional for fields and methods. `private` fields and methods can only be read or written from methods declared on the same class. Constructors are named `constructor(...)` and classes are called directly; there is no `new` keyword.

Field initializers are evaluated once per instance before the constructor runs. They may use `this` to read fields initialized earlier in the class body:
```js
class Derived {
    public x = 4 + 1
    private y = this.x * 2
}
```
Class methods and field initializers can close over surrounding local variables.

Inside class methods and field initializers, `@name` is shorthand for `this.name`. Constructor parameters may use `@name` to assign matching fields before the constructor body runs. `with expression { ... }` blocks can also use `@name` against the active receiver. See `doc/syntax.md` for the syntax rules.

Classes can declare public or private getters and setters:
```js
class Person {
    private _name = ""

    public get name() {
        return this._name
    }

    public set name(value) {
        this._name = value
    }
}

var person = Person()
person.name = "Ada"
io.print(person.name)
```
Getters must have no user parameters. Setters must have exactly one user parameter. Class members cannot share names, except for one getter and one setter for the same property.

See `doc/classes.md` for the full class model, lookup rules, initializer behavior, and v1 boundaries.

---
## Async
Little supports JS-style Promise and timer primitives:
```js
Promise(fn(resolve, reject) {
    setTimeout(fn() { resolve("done") }, 10)
}).next(fn(value) {
    io.print(value)
}).catch(fn(reason) {
    io.print(reason)
}).finally(fn() {
    io.print("settled")
})
```

`task.run(callable, state)` runs a Little function in a worker VM on a host thread and returns a promise. Only explicit `state` crosses the task boundary. Tables and arrays in `state` are shared through proxies, while scalars are copied. See `doc/std/task.md` for the full task model and limitations.

Scripts can manage async work explicitly with `mainloop.run()`, `mainloop.poll()`, and `mainloop.stop()`.

`async fn` creates an asynchronous function. Calling it returns a promise immediately; the function body is run by the VM event loop. `await` is only valid inside async functions and waits for a promise before continuing:
```js
var delay = fn(value) {
    return Promise(fn(resolve, reject) {
        setTimeout(fn() { resolve(value) }, 10)
    })
}

var work = async fn() {
    var value = await delay(7)
    return value + 1
}

work().next(fn(value) {
    io.print(value)
})
```

---
## Error Handling
`pcall(fn [, args...])` calls a function and returns a result table instead of letting runtime errors escape:
```js
var result = pcall(fn() {
    return 1 + "bad"
})

if result.ok {
    io.print(result.value)
}
else {
    io.print(result.error)
}
```

---
## Limits
Little has fixed VM stack and parser/compiler limits by design. See `doc/limits.md` for the current defaults, including stack depth, call depth, argument counts, branch-chain limits, and multi-return limits.
