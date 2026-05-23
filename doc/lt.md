# little - language overview
## Types
Little supports a set of basic types:
* `null` - represents the absense of a value
* `number` - a double-precision floating point number
* `boolean` - either true or false
* `string` - a reference to an immutable string
* `function` - a little-defined function
* `closure` - any function that captures surroudning values
* `array` - 0-indexed array of values
* `table` - a table of key-value pairs
* `promise` - an asynchronous value that can be fulfilled or rejected
* `native` - reference to a natively defined C function
* `ptr` - userdata pointer set by C api

These are grouped into `Value` and `Object` types, which are passed by value and reference respectively
`null`, `number`, `boolean`, and `string` are the `Value` types. String is special in that they are immutable and stored in a global deduplication table, and the actual value passed around is an index into that.

---

## Language statements
### var
```js
var a
var b = 10
var c = (a or 10) + b
```
Variables are declared with the `var` keyword. Only a single name is permitted per `var` statement, with an optional expression following the `=` assignment operator.

---
### if
```js
if a is 100 { ... }
elseif a is 150 { ... }
elseif b is a { ... }
else { ... }
```
Branching is done with the `if` statement, followed by an expression to evaluate and then a mandatory set of braces, containing the body to execute. `if`s can be followed by any number of `elseif` statements, and optionally a final `else`statement,

---
### for
```js
var a = [ 100, 200, 300 ]
for item in array.each(a) { ... }
```
`for` loops come in only one flavour in little, requireing a single identifier to be the loop variable, and an expression that evaluates into an iterator function. It will be repeatedly called - and it's result stored in the loop variable - until it evaluates to null.

---
### while
```js
var a = 0
while a < 10 { a = a + 1 }
```
`while` loops continually evaluate their condition and execute their bodies.

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
```
`return` exits the current execution frame, and returns a single value to the caller.

---
### assignment
```js
var a = 10
a = 20
```
Any identifier followed by `=` assignment.

---
Any top-level statement that doesn't match any of these is instead executed as an `expression`

---
## Language expressions
Expressions consist of all literals and operators.

---
### Literals
* `null` is both a type and a literal value
* `number` literals are any decimal number strings - `123`, `0.5`, `123.123` etc
* `boolean` literals are either `true` or `false`
* `string` literals are any double-quoted strings - `"hello world!"`, `"i love apples"`
* `array` literals are a list of values between brackets - `[ 1, true, null, "banana" ]`
* `table` literals are `key: value` pairs grouped between braces - `{ a: 10 b: 20 c: true }`
* `function` literals are declared with this syntax: `var my_fn = fn(a, b) { return a + b }`
    * They are first-class objects, and can only be stored through assignment
    * Can be trivially passed as parameters as well
    * Parameter list is mandatory, even if empty
* `async fn` literals return a promise when called, and may use `await`
* `class` declarations create callable class objects that construct instances
### Operators
The mathematical operators `+`, `-`, `*`, and `/` only operator on `number` values
The comparison operators `<`, `<=`, `>`, `>=` also only work with `number`s
The comparison operators `is` and `isnt` work on all types
The logical operators `or`, `and` and `not` compare values based on their `truthiness`, and return their last operand
The index operator `[expression]` works on any `table` and `array` values
The dot operator `.` is syntax sugar for indexsing `table`s - `my_table.my_index = 10`
The colon operator `:` is syntax sugar for receiver-passing method calls - `obj:method(a)` is equivalent to `obj.method(obj, a)`. Methods conventionally name the first parameter `this`.

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
Field initializers do not close over surrounding local variables in v1.

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

`thread.run(source, state)` runs Little source text in an isolated worker VM and returns a promise. The worker receives a copied `state` global and resolves with the script's returned value. Only `null`, numbers, booleans, strings, arrays, and tables cross worker boundaries.

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
