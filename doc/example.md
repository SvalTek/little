# C API examples

Here are some samples of how to extend the language through the C API. For brevity, these all assume you've already opened a `lt_VM*` by the name vm.

## Native function binding
```c
    uint8_t my_add(lt_VM* vm, uint8_t argc)
    {
        if(argc != 2) lt_runtime_error(vm, "Expected two arguments!");
        lt_Value right = lt_pop(vm);
        lt_Value left = lt_pop(vm);

        if(!LT_IS_NUMBER(left) || !LT_IS_NUMBER(right))
            lt_runtime_error(vm, "Invalid types!");

        lt_push(vm, lt_make_number(lt_get_number(left) + lt_get_number(right)));
        return 1; // we have pushed to the stack!
    }

    lt_table_set(vm, vm->global, lt_make_string(vm, "add"), lt_make_native(vm, my_add));
```

## Native module binding

```c
    lt_Value my_module = lt_make_table(vm);

    lt_table_set(vm, my_module, lt_make_string(vm, "func1"), lt_make_native(vm, my_func1));
    lt_table_set(vm, my_module, lt_make_string(vm, "func2"), lt_make_native(vm, my_func2));
    lt_table_set(vm, my_module, lt_make_string(vm, "func3"), lt_make_native(vm, my_func3));

    lt_table_set(vm, vm->global, lt_make_string(vm, "module"), my_module);
```

## Native library loading

A native library is a platform-shared library (`.dll` on Windows, `.so` on Linux,
`.dylib` on macOS) that exports an `ltopen` function:

```c
    #include "little.h"

    #ifdef _WIN32
    #define LT_NATIVE_EXPORT __declspec(dllexport)
    #else
    #define LT_NATIVE_EXPORT __attribute__((visibility("default")))
    #endif

    LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* lt)
    {
        if (!lt || lt->version != LT_API_VERSION || lt->size < sizeof(lt_Api))
            return LT_VALUE_NULL;

        lt_Value my_module = lt->make_table(vm);

        lt->table_set(vm, my_module, lt->make_string(vm, "func1"), lt->make_native(vm, my_func1));
        lt->table_set(vm, my_module, lt->make_string(vm, "func2"), lt->make_native(vm, my_func2));

        return my_module;
    }
```

Little loads that library with `loadLibrary(path)`. The returned value is the
value returned by `ltopen`, normally a table of native functions:

```js
    var module = loadLibrary("native/my_module")
    module.func1()
```

`loadLibrary` is separate from `import`: `import` loads Little source modules,
while `loadLibrary` loads native code. It uses the same registered module search
paths as `import`, but tries the platform native-library extension instead of
`.little`.

Native libraries call Little through the `lt_Api` table passed to `ltopen`.
This keeps libraries portable on Windows and Unix-like platforms without linking
the library against `little.exe`. The `lt_Api` table is the ABI boundary:
libraries should validate the pointer, version, and size before using it.

The project includes an optional JSON native library as a larger example:

```js
    var json = loadLibrary("nativelib/json/build/json")

    var data = json.parse("{\"name\":\"Ada\",\"ok\":true}")
    io.print(data.name)

    io.print(json.stringify([ "Ada", 2, true, null ]))
```

Build systems or test scripts must compile the native library for the current
platform before loading it. The included JSON library can be built with:

```powershell
    task build:nativelibs
```

## Optional library loading

```c
    ltstd_open_all(vm);    // pcall, io, math, array, table, string, gc
    ltstd_open_loadlib(vm); // optional loadLibrary for native code
    ltasync_open_all(vm);  // Promise, timers, task.run
```

The CLI opens the standard, native-library, and async libraries, but embedders
can choose only the modules they want. Omit `ltstd_open_loadlib(vm)` when scripts
must not be able to load native code.
