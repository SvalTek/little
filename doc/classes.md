# little classes

Classes are declaration-style object templates. They are intentionally small: single inheritance is supported, but there is no `new`, no static members, no class expressions, no mixins, and no multiple inheritance.

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
2. Run superclass field initializers, then this class's field initializers, each in class-body order.
3. Run this class's `constructor(...)`, if present. If a subclass has no constructor, the superclass constructor is called with the original arguments.
4. Return the instance.

Constructors are named `constructor(...)`. There is no `new` keyword. A constructor cannot be marked `public` or `private`.

## Inheritance

Use `extends` with a superclass identifier for single inheritance:

```js
class Animal {
    constructor(@name) {
    }

    speak() {
        return string.concat("animal:", @name)
    }
}

class Dog extends Animal {
    constructor(name) {
        super(name)
    }

    override speak() {
        return string.concat(super.speak(), ":dog")
    }
}
```

The superclass must be named directly. Dynamic superclass expressions are not supported.

Inherited public methods, getters, setters, and fields participate in lookup. A subclass member that shares a public inherited name is an error unless it is an explicit `override`, and `override` must match an inherited member of the same kind. Fields cannot be marked `override`.

Inside a constructor, `super(...)` calls the superclass constructor for the current instance. Inside methods and constructors, `super.method(...)` calls an inherited public method with the current instance as `this`.

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
io.print(box.secret) ; null
io.print(box:read()) ; hidden
```

This is runtime-enforced privacy for Little code. It is not intended as a security boundary against native C API code.

Private members are class-private, not inherited-private. A subclass may declare a private member with the same name as a superclass private member; superclass methods see the superclass private member, and subclass methods see the subclass private member.

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

Inside class methods, `@field` is shorthand for `this.field`:

```js
class Counter {
    public value = 0

    public add(amount) {
        @value = @value + amount
        return @value
    }
}
```

Constructors may prefix parameters with `@` to assign matching fields before the constructor body runs:

```js
class Person {
    constructor(@name, @score) {
        @score = @score + 1
    }
}
```

This behaves like assigning `this.name = name` and `this.score = score` at the start of the constructor.

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

Field initializers may also close over surrounding local variables:

```js
var seed = 4

class Seeded {
    value = seed + 1
}
```

Use constructor parameters when each instance should receive a different value from the caller:

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

1. Private getter for the currently executing class, only from methods of that class.
2. Public getter, walking from the instance class up through superclasses.
3. Private field for the currently executing class, only from methods of that class.
4. Public field.
5. Private method for the currently executing class, only from methods of that class.
6. Public method, walking from the instance class up through superclasses.
7. `null`.

Writing `obj.name = value` checks, in order:

1. Private setter for the currently executing class, only from methods of that class.
2. Public setter, walking from the instance class up through superclasses.
3. Existing private field for the currently executing class, only from methods of that class.
4. Public field assignment.

This means assigning an unknown public property creates or updates a public field on that instance.

## V1 Boundaries

Not implemented in v1:

* static fields or methods
* class expressions
* `new`
* external class reopening such as `fn MyClass:method(...) { ... }`
* `protected`
* dynamic superclass expressions
