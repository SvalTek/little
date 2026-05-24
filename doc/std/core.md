# core

## pcall

`pcall(fn [, args...])` calls `fn` with optional arguments and returns a result table instead of letting runtime errors escape.

On success:

```js
{ ok: true value: result }
```

On failure:

```js
{ ok: false error: "message" }
```

Trapped errors are not sent to the host error callback.

## unpack

`unpack(array [, start [, end]])` returns array elements as positional return values.

Indexes are 0-based, and `end` is inclusive:

```js
unpack([ 10, 20, 30 ])       // 10, 20, 30
unpack([ 10, 20, 30 ], 1)    // 20, 30
unpack([ 10, 20, 30 ], 1, 1) // 20
```

`start` defaults to `0`. `end` defaults to the last array index. Bounds must be numbers, and the first argument must be an array.

`unpack` expands in `return unpack(values)` and when it is the final call argument:

```js
return unpack(values)
fn("prefix", unpack(values))
```

In scalar contexts, only the first returned value is used.

## import

`import "path"` loads a Little module and returns its exported value.

```js
var greeter = import "tests/fixtures/greeter"
io.print(greeter.greet("Ada"))
```

Named import syntax destructures that returned module value into locals:

```js
import { greet, shout } from "tests/fixtures/greeter"
```

Module search paths can be registered with `module.addPath(...)`:

```js
module.addPath("lib")
var common = import "utils/common"
```

For each direct path or search-path candidate, Little tries `path.little`, then `path/init.little`. Extensionless files are not loaded. Use `module.clearPaths()` to remove registered search paths.

When a search path contains `?`, it receives the first import path segment and any remaining subpath is appended after the template.

See [../modules.md](../modules.md) for module loading, exporting, path, and cache behavior.
