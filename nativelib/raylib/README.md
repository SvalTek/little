# Little raylib native library (PoC)

This optional native library wraps a small subset of
[raylib](https://github.com/raysan5/raylib) for 2D windows.

It is not part of Little's core language or standard library. Scripts opt into
it explicitly with `loadLibrary(...)`, and embedders can omit `loadLibrary`
support entirely if native library loading should be blocked.

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

CI cannot open windows, so the e2e test (`tests/e2e/native-raylib.little`)
only asserts the load surface and error paths. Windowed behavior is exercised
manually with `scripts/raylib/demo.little`.

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
    ray.clear({ r: 20, g: 20, b: 30 })
    ray.rect(10, 10, 120, 40, { r: 255, g: 100, b: 50 })
    ray.text("hello", 10, 60, 20, ray.white)
})

while ray.update() {
}

ray.close()
```

`update()` runs `start` once, then per frame: collects draw commands from the
`update` callback, replays them inside `BeginDrawing`/`EndDrawing`, polls
Little async work (`lt->poll`), and returns whether the window is still open.
Draw helpers only buffer commands for the current frame (cap 4096); windows and
GL calls all stay on the calling thread.

## API

| Little API | Meaning |
| --- | --- |
| `open(width, height, title)` | Open the single process window |
| `close()` | Close the window if open |
| `on("start" \| "update", fn)` | Register lifecycle callbacks |
| `update()` | Run one frame; returns `false` when the window should close |
| `clear({ r, g, b [, a] })` | Set this frame's background color |
| `rect(x, y, w, h, color)` | Queue a filled rectangle for this frame |
| `text(str, x, y, size, color)` | Queue text for this frame |
| `keyPressed(code)` | Whether a key was pressed this frame (`ray.keys.*`) |
| `keyDown(code)` | Whether a key is held down |
| `mousePressed(button)` | Whether a mouse button was pressed |
| `mouse()` | Current mouse position as `{ x, y }` |
| `setFPS(n)` | Cap the frame rate |
| `version` | Vendored raylib version string |
| `keys` | Key/mouse code table (`space`, `enter`, `escape`, `left/right/up/down`, `wasd`, `mouseLeft/Right/Middle`) |
| `white`, `black` | Convenience color tables |

One window per process: loading the library into a second VM is rejected,
matching the webui library. Colors are `{ r, g, b }` tables with 0-255
channels and an optional `a`.
