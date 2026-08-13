# little_std
The Little standard library is divided into modules. Embedders can load the normal stdlib with `ltstd_open_all(vm)` or open individual modules with the `ltstd_open_*` functions. Native library loading is opt-in through `ltstd_open_loadlib(vm)`.

## Modules
- [core](std/core.md) - `pcall`, `unpack`, `import`, and `loadLibrary`
- [io](std/io.md)
- [math](std/math.md)
- [array](std/array.md)
- [table](std/table.md)
- [string](std/string.md)
- [gc](std/gc.md)
- [async](std/async.md)
- [task](std/task.md)
