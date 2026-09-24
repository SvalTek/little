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

`import "path"` only loads Little source files. It tries `path.little`, then `path/init.little`.

```js
import { greet } from "tests/fixtures/greeter"
```

If those direct candidates fail, Little tries each registered module search path. Register search paths with `module.addPath(...)`:

```js
module.addPath("lib", "vendor")
import { common } from "utils/common"
```

Plain search paths are treated as roots, so `module.addPath("lib")` makes `import "utils/common"` try:

```text
lib/utils/common.little
lib/utils/common/init.little
```

Search paths may contain `?` as a package-name placeholder. For `import "toolkit/math"`, `?` receives `toolkit` and the remaining `math` path is appended after the template:

```js
module.addPath("packages/?/src")
var math = import "toolkit/math"
```

That tries `packages/toolkit/src/math.little` and `packages/toolkit/src/math/init.little`. For `import "toolkit"`, the same search path tries `packages/toolkit/src.little` and `packages/toolkit/src/init.little`.

Use `module.clearPaths()` to remove registered search paths. Relative paths are still resolved by the host process in the same way as normal file opens, so they are relative to the current working directory.

## Host Module Loaders

Embedders can register a C module loader with `lt_add_module_loader(...)`.
Loaders are called in registration order and can provide source from a resource
that is not a file, such as an archive or an in-memory filesystem. A loader
returns either `LT_MODULE_LOADER_NOT_FOUND` or `LT_MODULE_LOADER_FOUND` and,
when it finds a module, supplies the source and an optional resolved module name.
The source and resolved name must be allocated with `vm->alloc`; the VM releases
them after loading.

Register custom loaders before `ltstd_open_all(vm)`. The standard library adds
the filesystem loader when it opens `import`, so registered loaders run before
the normal `path.little` and `path/init.little` lookup. The resolved module name
is used for diagnostics and cache identity, which lets archive-backed modules
retain useful source names without extracting files.

The same registered search paths are also used by `loadLibrary`, but native
libraries are loaded explicitly with `loadLibrary(...)`, not with `import`.
`import` remains source-only.

## Bundled Source

The CLI can package source files into a self-contained executable:

```text
little --bundle app/main.little --include app -o app.exe
```

`--bundle` selects the entry file. Each `--include` option adds a file or
directory; directory contents are stored using paths relative to that
directory. The bundled loader runs before filesystem lookup, so an embedded
module takes precedence over a module with the same path on disk. The original
source tree is not needed when the executable runs.

Bundling stores Little source, not native code. `loadLibrary(...)` continues to
use the host filesystem and native-library search paths, so native libraries
must still be supplied separately for the target platform.

## Cache Behavior

Imports are cached per VM by the resolved path spelling. Importing the same
spelling again returns the cached value without rerunning the module body.

Paths are not canonicalized. For example, `import "lib/tool"` and
`import "./lib/tool"` can resolve to the same file but have separate cache
entries, so use one consistent path spelling when module initialization must
run only once.

Caching starts before the module body runs, so simple import cycles can observe a partially initialized module value. The cached placeholder is replaced with the module's returned value after execution completes. If module execution fails, the placeholder is removed.

## Syntax Summary

```js
import "module"
import { name, otherName } from "module"
import { exportedName: localName } from "module"
```
