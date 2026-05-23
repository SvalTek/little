# little classes

Classes are declaration-style object templates. They are intentionally small in v1: no inheritance, no `new`, no static members, and no class expressions.

```js
class Counter {
    private value = 0
    public label = "counter"

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

## Construction

`class Name { ... }` declares a callable class object named `Name` in the current scope. Calling the class creates a new instance:

```js
var counter = Counter(2)
```

Construction order is:

1. Allocate a new instance.
2. Run field initializers in class-body order.
3. Run `constructor(...)`, if present.
4. Return the instance.

Constructors are named `constructor(...)`. There is no `new` keyword. A constructor cannot be marked `public` or `private`.

## Public And Private Members

Members are public by default. `public` is allowed for clarity, but optional:

```js
class Example {
    public a = 1
    b = 2
}
```

Private fields, methods, getters, and setters can only be accessed while executing a method declared by the same class:

```js
class SecretBox {
    private secret = "hidden"

    public read() {
        return this.secret
    }
}

var box = SecretBox()
io.print(box.secret) -- null
io.print(box:read()) -- hidden
```

This is runtime-enforced privacy for Little code. It is not intended as a security boundary against native C API code.

## Methods And This

Class methods have an implicit `this` local. Do not include `this` in the parameter list:

```js
class Counter {
    private value = 0

    add(amount) {
        this.value = this.value + amount
        return this.value
    }
}
```

Call methods with the existing `:` sugar:

```js
counter:add(3)
```

For class instances, `obj:method(a)` behaves like a receiver call and makes `this` available inside the method. Direct `obj.method(...)` is not the intended calling style for class methods because it does not pass the instance as `this`.

## Field Initializers

Field initializers are evaluated once per instance before the constructor runs:

```js
class Derived {
    public x = 4 + 1
    private y = this.x * 2

    value() {
        return this.y
    }
}
```

Initializers may use `this` to read fields initialized earlier in the class body. They are not reactive. If a later method changes `x`, `y` is not automatically recomputed.

Important v1 limitation: field initializers do not close over surrounding local variables.

```js
var seed = 4

class Bad {
    value = seed + 1 -- not supported as a captured local in v1
}
```

Use constructor parameters when a value needs to come from the surrounding program:

```js
class Good {
    value = 0

    constructor(seed) {
        this.value = seed + 1
    }
}
```

## Getters And Setters

Getters and setters define property-style access:

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

Rules:

* A getter has no user parameters.
* A setter has exactly one user parameter.
* A getter and setter with the same visibility/name may coexist.
* Duplicate getters with the same visibility/name are errors.
* Duplicate setters with the same visibility/name are errors.
* No class members may share a name, except one getter and one setter for the same property.

Private accessors follow the same privacy rule as private fields and methods:

```js
class Person {
    private _name = ""

    private get secret() {
        return this._name
    }

    public read() {
        return this.secret
    }
}
```

## Lookup Rules

Reading `obj.name` checks, in order:

1. Private getter, only from same-class methods.
2. Public getter.
3. Private field, only from same-class methods.
4. Public field.
5. Private method, only from same-class methods.
6. Public method.
7. `null`.

Writing `obj.name = value` checks, in order:

1. Private setter, only from same-class methods.
2. Public setter.
3. Existing private field, only from same-class methods.
4. Public field assignment.

This means assigning an unknown public property creates or updates a public field on that instance.

## V1 Boundaries

Not implemented in v1:

* inheritance
* static fields or methods
* class expressions
* `new`
* external class reopening such as `fn MyClass:method(...) { ... }`
* `protected`
* initializer capture of surrounding locals
