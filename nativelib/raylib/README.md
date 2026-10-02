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

`tests/e2e/native-raylib*.little` cover the load surface, value types,
CPU-side `Image` behavior, and error paths without needing a display.
`tests/windowed/native-raylib-draw.little` opens a hidden window and renders a
real frame (rect, text, textures, the default font, every draw variant), and
`tests/run-e2e.ps1` runs it whenever it can create a window, skipping it
otherwise; CI runs the Linux test job under `xvfb-run` so it executes there.
`scripts/raylib/demo.little` is the runnable windowed example.

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
Draw helpers only buffer commands for the current frame (cap 1024); windows and
GL calls all stay on the calling thread.

`open` accepts an optional fourth argument, a table of window options applied
before the window is created:

```js
ray.open(800, 450, "demo", { hidden: true, resizable: true, vsync: true })
```

| Option | Effect |
| --- | --- |
| `hidden` | Create the window hidden (useful for offscreen work) |
| `resizable` | Allow the user to resize the window |
| `vsync` | Enable vertical sync |
| `fullscreen` | Start in fullscreen |
| `undecorated` | Remove the window frame |
| `alwaysRun` | Keep rendering while minimized or unfocused |

A runtime error raised inside a `start` or `update` callback is reported but
does not stop the loop; the callback aborts for that frame and the next
`update()` continues. Scripts that drive the loop must therefore be careful
that an error cannot leave the window open forever.

The library emits raylib log lines on stdout. Tests and other output-sensitive
scripts should call `ray.traceLog(ray.log.none)` (or `ray.log.warning`) first.

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

## Resources

`Image`, `Texture`, and `Font` wrap raylib's resource structs. They are created
by loader functions rather than by calling the class, and each instance owns
its raylib object: the payload is released when the instance is unloaded or
collected, so no explicit free is required.

```js
var image = ray.genImageColor(16, 16, ray.colors.skyblue)   ; CPU only
var texture = ray.loadTextureFromImage(image)               ; needs a window
image:unload()                                              ; optional

texture:draw(ray.Vector2(10, 10), ray.colors.white)

var font = ray.loadFont("assets/Inter.ttf", 32)
font:draw("hello", ray.Vector2(10, 40), 24, 1, ray.colors.white)
io.print(font:measure("hello", 24, 1):toString())
```

`Image` lives in CPU memory, so `loadImage` and `genImageColor` work without a
window. `Texture` and `Font` need a live GL context: loading them without
`ray.open` raises, and `unload()` on a closed window simply drops the handle,
because the GPU objects died with the context.

| Type | Fields | Methods |
| --- | --- | --- |
| `Image` | `width`, `height` | `unload`, `export`, `toString` |
| `Texture` | `width`, `height` | `draw`, `drawRec`, `drawPro`, `unload`, `toString` |
| `Font` | `baseSize`, `glyphCount` | `measure`, `draw`, `unload`, `toString` |

| Loader | Source |
| --- | --- |
| `loadImage(path)` | Image file from disk |
| `genImageColor(width, height, color)` | Generated solid image |
| `loadTexture(path)` | Texture from an image file (window required) |
| `loadTextureFromImage(image)` | Texture from an `Image` (window required) |
| `loadFont(path, size)` | Font from a `.ttf`/`.otf` file (window required) |
| `defaultFont()` | raylib's built-in font (window required, never unloaded) |

`defaultFont()` returns raylib's built-in font as a `Font` instance. It is owned
by raylib rather than by the script, so `unload()` on it does nothing and
collecting it never frees the built-in font.

`texture:draw(position, tint)` draws the whole texture,
`texture:drawRec(source, position, tint)` draws a source rectangle, and
`texture:drawPro(source, dest, origin, rotation, tint)` draws with scaling and
rotation. `font:draw(text, position, size, spacing, tint)` and
`font:measure(text, size, spacing)` mirror `DrawTextEx`/`MeasureTextEx`;
`measure` returns a `Vector2`.

## API

| Little API | Meaning |
| --- | --- |
| `open(width, height, title [, options])` | Open the single process window |
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
| `windowSize()` | Window client size as a `Vector2` (window required) |
| `setFPS(n)` | Cap the frame rate |
| `traceLog(level)` | Set the raylib log threshold (`ray.log.*`) |
| `loadImage(path)`, `genImageColor(w, h, color)` | Create an `Image` |
| `loadTexture(path)`, `loadTextureFromImage(image)` | Create a `Texture` |
| `loadFont(path, size)`, `defaultFont()` | Create a `Font` |
| `Vector2`, `Vector3`, `Color`, `Rectangle` | Value type constructors |
| `Image`, `Texture`, `Font` | Resource types (created by their loaders) |
| `colors` | Named raylib palette (`lightgray` … `raywhite`, `blank`) |
| `keys` | Key/mouse code table (`space`, `enter`, `escape`, `left/right/up/down`, `wasd`, `mouseLeft/Right/Middle`) |
| `log` | Trace log levels (`all` … `none`) |
| `version` | Vendored raylib version string |

One window per process: loading the library into a second VM is rejected,
matching the webui library.
