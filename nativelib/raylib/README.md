# Little raylib native library

This optional native library wraps a subset of
[raylib](https://github.com/raysan5/raylib) for 2D windows. It is not part of
Little's core language or standard library. Scripts opt into it explicitly with
`loadLibrary(...)`, and embedders can omit `loadLibrary` support entirely if
native library loading should be blocked.

Raylib's value structs (`Vector2`, `Vector3`, `Color`, `Rectangle`) are exposed
as native-backed Little classes, so they can be produced by constructors, carry
methods, and be checked with `typeof`.

## Build

Raylib is vendored under `vendor/raylib`. Build after checkout with submodules:

```powershell
task build:nativelibs
```

The build creates `nativelib/raylib/build/raylib.dll` on Windows, or the
matching platform shared-library extension on other systems. It compiles the
vendored desktop sources (`rcore`, `rshapes`, `rtextures`, `rtext`, `rglfw`,
`rmodels`, `raudio`) plus `raylib_little.c` into one shared library.
`tests/run-e2e.ps1` builds the same library so `task test` works on a fresh
checkout.

On Linux the desktop build needs the GL/X11 development headers:

```sh
sudo apt-get install -y libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

CI cannot open windows, so the e2e tests (`tests/e2e/native-raylib*.little`)
only assert the load surface, value types, and error paths. Windowed behavior
is exercised manually with `scripts/raylib/demo.little`.

## Lifecycle

Everything raylib runs on the VM thread (OpenGL context affinity). The library
never blocks Little: the script owns the loop and pumps one frame per `update`.

```js
var ray = loadLibrary("nativelib/raylib/build/raylib")

ray.open(800, 450, "demo")
ray.on("start", fn() {
    io.print("ready")
})
ray.on("update", fn(dt) {
    ray.clear(ray.colors.darkblue)
    ray.rect(ray.Rectangle(10, 10, 120, 40), ray.Color(255, 100, 50))
    ray.text("hello", ray.Vector2(10, 60), 20, ray.colors.white)
})

while ray.update() {
}

ray.close()
```

`update()` runs `start` once, then per frame: collects draw commands from the
`update` callback, replays them inside `BeginDrawing`/`EndDrawing`, polls
Little async work (`lt->poll`), and returns whether the window is still open.
A callback that calls `close()` ends the loop cleanly; the frame is not drawn.
Draw helpers only buffer commands for the current frame (cap 4096); windows and
GL calls all stay on the calling thread.

## Value types

`Vector2`, `Vector3`, `Color`, and `Rectangle` are native-backed classes. Each
instance holds the raylib struct as its native payload, freed by the collector.

```js
var position = ray.Vector2(10, 20)
var from_table = ray.Vector2 { x: 1, y: 2 }
var tint = ray.Color { r: 255, g: 100, b: 50 }   ; alpha defaults to 255

position.x = 30          ; fields are mutable
var moved = position:add(ray.Vector2(1, 1))      ; operations return new values
io.print(moved:toString())                        ; (31, 21)
io.print(typeof position is ray.Vector2)          ; true
io.print(type position)                           ; instance
```

Constructors accept either positional numbers or a single table, and fill
missing fields with defaults (`Color` alpha defaults to `255`, other fields to
`0`). Fields are mutable; the math methods return new instances and leave the
receiver unchanged. Because instances are ordinary Little reference values,
`var alias = position` aliases the same instance, exactly like a table.

`is` compares instances by identity, so equal vectors are not `is`-equal:

```js
io.print(ray.Vector2(1, 2) is ray.Vector2(1, 2))          ; false
io.print(ray.Vector2(1, 2):equals(ray.Vector2(1, 2)))     ; true
```

| Type | Fields | Methods |
| --- | --- | --- |
| `Vector2` | `x`, `y` | `add`, `sub`, `mul`, `scale`, `dot`, `length`, `lengthSq`, `normalize`, `distance`, `clone`, `equals`, `toString` |
| `Vector3` | `x`, `y`, `z` | `add`, `sub`, `mul`, `scale`, `dot`, `cross`, `length`, `lengthSq`, `normalize`, `distance`, `clone`, `equals`, `toString` |
| `Color` | `r`, `g`, `b`, `a` | `equals`, `withAlpha`, `toString` |
| `Rectangle` | `x`, `y`, `width`, `height` | `equals`, `contains`, `center`, `toString` |

Color channels are clamped to `0`-`255`. `mul` is component-wise; `scale`
takes a number. `toString` formats as `(x, y)`, `(x, y, z)`, `(x, y, w, h)`, or
`rgba(r, g, b, a)`.

## API

| Little API | Meaning |
| --- | --- |
| `open(width, height, title)` | Open the single process window |
| `close()` | Close the window if open |
| `on("start" \| "update", fn)` | Register lifecycle callbacks |
| `update()` | Run one frame; returns `false` when the window should close |
| `clear(color)` | Set this frame's background color |
| `rect(bounds, color)` | Queue a filled rectangle for this frame |
| `text(str, position, size, color)` | Queue text for this frame |
| `keyPressed(code)` | Whether a key was pressed this frame (`ray.keys.*`) |
| `keyDown(code)` | Whether a key is held down |
| `mousePressed(button)` | Whether a mouse button was pressed |
| `mouse()` | Current mouse position as a `Vector2` |
| `setFPS(n)` | Cap the frame rate |
| `Vector2`, `Vector3`, `Color`, `Rectangle` | Value type constructors |
| `colors` | Named raylib palette (`lightgray` … `raywhite`, `blank`) |
| `keys` | Key/mouse code table (`space`, `enter`, `escape`, `left/right/up/down`, `wasd`, `mouseLeft/Right/Middle`) |
| `version` | Vendored raylib version string |

One window per process: loading the library into a second VM is rejected,
matching the webui library.
