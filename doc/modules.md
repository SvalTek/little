# Little Modules

Little modules are ordinary Little source files loaded with `import`. A module runs once per VM, and later imports of the same resolved path return the cached module value.

## Exporting Values

Modules export by returning a value. The common shape is a table:

```js
fn greet(name) {
    return string.format("hello %s", name)
}

fn shout(name) {
    return string.upper(greet(name))
}

return {
    greet
    shout
}
```

Top-level `var`, `fn`, and `async fn` declarations inside the module are local to that module. Use `global` only for deliberate VM-wide state.

If a module returns no value, its cached module value is `true`.

## Importing A Module

The full module value can be imported as an expression:

```js
var greeter = import "greeter"
io.print(greeter.greet("Ada"))
```

Named import syntax destructures the returned module table into locals:

```js
import { greet, shout } from "greeter"

io.print(greet("Ada"))
io.print(shout("Ada"))
```

This behaves like importing the module value and destructuring it:

```js
var { greet, shout } = import "greeter"
```

The named form is preferred when a script only needs specific exported values.

## Paths

`import "path"` first opens the path exactly as written. If that fails, Little tries the same path with `.little` appended.

```js
import { greet } from "tests/fixtures/greeter"
```

At this stage, paths are resolved by the host process in the same way as normal file opens. They are not package names, and there is no package search path.

## Cache Behavior

Imports are cached per VM by resolved path. Importing the same file again returns the cached value without rerunning the module body.

Caching starts before the module body runs, so simple import cycles can observe a partially initialized module value. The cached placeholder is replaced with the module's returned value after execution completes.

## Syntax Summary

```js
import "module"
import { name, otherName } from "module"
import { exportedName: localName } from "module"
```
