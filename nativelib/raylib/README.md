# Little raylib native library

This optional native library wraps a subset of
[raylib](https://github.com/raysan5/raylib) for 2D and 3D windows. It is not part of
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

Release packages ship it as `libs/raylib/raylib.<ext>`, so scripts running from
an unpacked release load `loadLibrary("libs/raylib/raylib")` instead.

On Linux the desktop build needs the GL/X11 development headers:

```sh
sudo apt-get install -y libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

`tests/e2e/native-raylib*.little` cover the load surface, value types,
CPU-side `Image` behavior, and error paths without needing a display.
`tests/windowed/native-raylib-draw.little` opens a hidden window and renders a
real 2D frame (rect, text, textures, the default font, every draw variant), and
`tests/windowed/native-raylib-3d.little` generates meshes, renders a 3D scene
through a camera and a render texture, and checks the mode-balance errors, and
`tests/windowed/native-raylib-shaders.little` loads shaders, configures a
material, and asserts that the lighting shader changes rendered pixels.
`tests/run-e2e.ps1` runs the windowed tests whenever it can create a window,
skipping them otherwise; CI runs the Linux test job under `xvfb-run` so they
execute there. `scripts/raylib/demo.little` and `scripts/raylib/demo3d.little`
are the runnable windowed examples.

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
Little async work, and returns whether the window is still open. Async work is
polled without sleeping (`poll_now`), so a pending `setTimeout` never stalls the
frame; the return value reflects the window state *after* polling, so a timer
callback that calls `close()` ends the loop cleanly. Draw helpers only buffer
commands for the current frame (cap 1024); windows and GL calls all stay on the
calling thread. `clear` and `screenshot` are queued the same way, so a clear
lands inside whatever render target is active where it was called, and a
screenshot captures the frame it was queued in rather than a stale buffer.

A frame whose draw commands leave `beginMode2D`, `beginMode3D`,
`beginTextureMode`, or `beginShaderMode` unclosed is rejected before anything
is drawn.

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
| `Vector2` | `x`, `y` | `add`, `sub`, `mul`, `scale`, `dot`, `length`, `lengthSq`, `normalize`, `distance`, `rotate`, `lerp`, `reflect`, `clone`, `equals`, `toString` |
| `Vector3` | `x`, `y`, `z` | `add`, `sub`, `mul`, `scale`, `dot`, `cross`, `length`, `lengthSq`, `normalize`, `distance`, `lerp`, `clone`, `equals`, `toString` |
| `Color` | `r`, `g`, `b`, `a` | `equals`, `withAlpha`, `toString` |
| `Rectangle` | `x`, `y`, `width`, `height` | `equals`, `contains`, `center`, `toString` |
| `Camera2D` | `offset`, `target` (`Vector2`), `rotation`, `zoom` | `toString` |
| `Camera3D` | `position`, `target`, `up` (`Vector3`), `fovy`, `projection` | `toString` |
| `Ray` | `position`, `direction` (`Vector3`) | `toString` |
| `RayCollision` | `hit`, `distance`, `point`, `normal` | `toString` |

Color channels are clamped to `0`-`255`. `mul` is component-wise; `scale`
takes a number. `toString` formats as `(x, y)`, `(x, y, z)`, `(x, y, w, h)`, or
`rgba(r, g, b, a)`.

`Camera2D` defaults to `offset (0, 0)`, `target (0, 0)`, `rotation 0`, `zoom 1`.
It is built from a table, from `offset` and `target`, or from
`offset, target, rotation, zoom`:

```js
var camera = ray.Camera2D { target: ray.Vector2(0, 0), offset: ray.Vector2(400, 225), zoom: 2 }
ray.beginMode2D(camera)
ray.rect(ray.Rectangle(0, 0, 32, 32), ray.colors.red)
ray.endMode2D()
```

`beginMode2D` and `endMode2D` must be balanced inside a frame; an unmatched
`endMode2D` is an error.

`Camera3D` defaults to `position (0, 0, 0)`, `target (0, 0, 0)`, `up (0, 1, 0)`,
`fovy 45`, and `projection perspective`. It is built from a table, from
`position` and `target`, or from `position, target, up, fovy, projection`:

```js
var camera = ray.Camera3D {
    position: ray.Vector3(10, 10, 10)
    target: ray.Vector3(0, 0, 0)
    fovy: 60
}
ray.beginMode3D(camera)
ray.cube(ray.Vector3(0, 0, 0), ray.Vector3(1, 1, 1), ray.colors.red)
ray.endMode3D()
```

`projection` is `ray.projection.perspective` or
`ray.projection.orthographic`, and `beginMode3D`/`endMode3D` must be balanced
like their 2D counterparts.

`Ray` holds a `position` and a `direction`. `raycastSphere(ray, center, radius)`
and `raycastBox(ray, min, max)` return a `RayCollision` exposing `hit`,
`distance`, `point`, and `normal`; they are pure math and need no window, so
they work in scripts that never open one.

```js
var cast = ray.Ray(ray.Vector3(0, 0, 0), ray.Vector3(0, 0, 1))
var collision = ray.raycastSphere(cast, ray.Vector3(0, 0, 5), 1)
io.print(collision.hit)             ; true
io.print(collision.distance)        ; 4
```

`getWorldToScreen(position, camera)` and `getScreenToWorldRay(position, camera)`
convert through a `Camera3D` and need a window.

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

A queued draw keeps its resource alive until the frame is replayed, and
`unload()` called in the same frame as a draw is deferred until after that
replay, so `font:draw(...)` followed by `font:unload()` is safe. Dropping the
last script reference to a resource mid-frame is safe for the same reason.

That is implemented with the module's internal `__queued` array (the same
mechanism as `__callbacks`): a command adds its texture or font when it is
queued, and the array is replaced once the frame is cleared. `tests/windowed/`
asserts on that array, because a resource freed mid-frame fails silently rather
than raising, so keep the name if the internals are ever reshuffled.

| Type | Fields | Methods |
| --- | --- | --- |
| `Image` | `width`, `height` | `unload`, `export`, `colorAt`, `toString` |
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

`image:colorAt(x, y)` returns one pixel as a `Color`. It needs a loaded image
and coordinates inside its bounds, and it is how a script inspects what a
render pass produced.

`texture:draw(position, tint)` draws the whole texture,
`texture:drawRec(source, position, tint)` draws a source rectangle, and
`texture:drawPro(source, dest, origin, rotation, tint)` draws with scaling and
rotation. `font:draw(text, position, size, spacing, tint)` and
`font:measure(text, size, spacing)` mirror `DrawTextEx`/`MeasureTextEx`;
`measure` returns a `Vector2`.

### Meshes, models, and render textures

`Mesh`, `Model`, and `RenderTexture` wrap raylib's 3D resources. raylib uploads
a mesh as soon as it generates or loads it, so everything in this section needs
a window, unlike `Image`.

```js
var mesh = ray.genMeshCube(1, 1, 1)
io.print(mesh:toString())             ; mesh(24 vertices, 12 triangles)

var model = ray.modelFromMesh(mesh)   ; the model owns the mesh now

var target = ray.loadRenderTexture(320, 180)
ray.beginTextureMode(target)
ray.beginMode3D(camera)
ray.cube(ray.Vector3(0, 0, 0), ray.Vector3(1, 1, 1), ray.colors.red)
model:draw(ray.Vector3(0, 0, 0), 1, ray.colors.white)
ray.endMode3D()
ray.endTextureMode()
target:draw(ray.Vector2(0, 0), ray.colors.white)
```

| Type | Fields | Methods |
| --- | --- | --- |
| `Mesh` | `vertexCount`, `triangleCount` | `unload`, `toString` |
| `Model` | `meshCount`, `materialCount` | `draw`, `drawEx`, `drawWires`, `drawWiresEx`, `unload`, `toString` |
| `RenderTexture` | `width`, `height` | `draw`, `drawPro`, `image`, `unload`, `toString` |

| Loader | Source |
| --- | --- |
| `genMeshCube(width, height, length)` | Cuboid mesh |
| `genMeshSphere(radius, rings, slices)` | Sphere mesh |
| `genMeshPlane(width, length, resX, resZ)` | Subdivided plane mesh |
| `genMeshCylinder(radius, height, slices)` | Cylinder mesh |
| `genMeshTorus(radius, size, radSeg, sides)` | Torus mesh |
| `genMeshKnot(radius, size, radSeg, sides)` | Trefoil knot mesh |
| `loadModel(path)` | Model from a `.gltf`, `.obj`, or `.iqm` file |
| `modelFromMesh(mesh)` | Model with a default material from a `Mesh` |
| `loadRenderTexture(width, height)` | Render target |

`rings`, `slices`, `resX`, `resZ`, `radSeg`, and `sides` size raylib's vertex
buffers, so they must be whole numbers between 1 and 1024; anything else is an
error.

`modelFromMesh` takes ownership of the mesh: the model frees it, so `unload()`
on that `Mesh` instance does nothing afterwards and passing the same mesh to a
second `modelFromMesh` is an error. `Model` draws mirror raylib's
`DrawModel`/`DrawModelEx` — `draw(position, scale, tint)` and
`drawEx(position, rotationAxis, angle, scale, tint)` — plus the wireframe
variants.

Render textures are stored bottom-up, so `draw(position, tint)` and
`drawPro(dest, origin, rotation, tint)` flip the source for you; `drawPro`
scales the whole target into `dest`. `image()` reads the target's colour
attachment back into an `Image`, following the same layout, so the last row of
that image is the top of the rendered scene. Unlike the draw helpers it is not
queued: it reads immediately, so calling it inside an update callback returns
the last completed frame rather than the one being built.

`beginTextureMode` and `beginMode3D` nest, so a frame can render a 3D scene into
a texture and then draw that texture on screen. All three modes must be balanced
before the frame ends.

## Drawing

Draw helpers queue commands for the current frame and take either the value
types or plain numbers, so game code does not build a `Rectangle` or `Vector2`
for every call:

```js
ray.rect(ray.Rectangle(10, 10, 40, 20), ray.colors.red)
ray.rect(10, 10, 40, 20, ray.colors.red)          ; the same draw
ray.circle(ray.Vector2(80, 40), 12, ray.colors.lime)
ray.circle(80, 40, 12, ray.colors.lime)
ray.text("hud", 8, 8, 14, ray.colors.white)
ray.textCentered("centered on the window", 100, 20, ray.colors.white)
ray.drawFPS(8, 8)
```

`textCentered(text, y, size, color)` centres on the window width, and
`textCentered(text, x, y, size, color)` centres on `x`; both measure raylib's
default font for you. `drawFPS(x, y)` queues raylib's frame-rate counter. The
value-type form remains available everywhere, including the helpers without a
numeric overload (`lineEx`, `triangle*`, `polygon*`, `ring`).

## 3D shapes

These queue draws for the current frame, like the 2D helpers. They must sit
inside `beginMode3D`/`endMode3D`: without a camera mode the draws would replay
against the default matrices and silently render nothing, so they raise
instead. `Model:draw` and `billboard` are checked the same way.

| Helper | Meaning |
| --- | --- |
| `cube(position, size, color)` / `cubeWires(position, size, color)` | Cuboid; `size` is a `Vector3` |
| `sphere(center, radius, color)` | Sphere |
| `sphereWires(center, radius, rings, slices, color)` | Sphere outline |
| `cylinder(position, radiusTop, radiusBottom, height, sides, color)` / `cylinderWires(...)` | Cylinder or cone |
| `capsule(start, end, radius, rings, slices, color)` | Capsule between two points |
| `plane(center, size, color)` | XZ plane; `size` is a `Vector2` |
| `triangle3D(a, b, c, color)` | Triangle in world space |
| `line3D(start, end, color)` / `point3D(position, color)` | Line / point |
| `boundingBox(min, max, color)` | Wire box between two corners |
| `grid(slices, spacing)` | Ground grid centered on the origin |
| `billboard(camera, texture, position, scale, tint)` | Camera-facing texture |

## Shaders and materials

`Shader` wraps a GLSL program and `Material` wraps raylib's material struct.
Both need a window, and both follow the ownership rule raylib uses for models: a
shader or texture handed to a material belongs to whoever created it, so
`unload()` on a material releases only its map array.

```js
var lighting = ray.lightingShader()          ; built-in directional light
lighting:setVector3("lightDirection", ray.Vector3(0.4, 0.8, 0.2))
lighting:setVector3("lightColor", ray.Vector3(1, 1, 1))
lighting:setVector3("ambientColor", ray.Vector3(0.25, 0.25, 0.3))

var material = ray.loadMaterialDefault()
material.shader = lighting
material:setColor(ray.materialMap.diffuse, ray.Color(255, 180, 120))

var model = ray.modelFromMesh(ray.genMeshCube(1, 1, 1))
model:setMaterial(0, material)               ; copies the maps into the model
model:draw(ray.Vector3(0, 0, 0), 1, ray.colors.white)
```

`lightingShader()` is a small directional light built on raylib's default
attribute and uniform names, so `DrawModel*` supplies the model matrices and
the material tint itself. It exposes `lightDirection` (pointing from the surface
towards the light), `lightColor`, and `ambientColor`.

Shaders can come from files or from source. `loadShaderFromMemory` is how a
bundled script ships one, since a packaged executable has no loose shader files
beside it:

```js
var shader = ray.loadShader("basic.vs", "basic.fs")        ; nil uses raylib's default
var fromSource = ray.loadShaderFromMemory(nil, fsSource)   ; fragment only
```

| Type | Fields | Methods |
| --- | --- | --- |
| `Shader` | - | `setFloat`, `setInt`, `setVector2`, `setVector3`, `setColor`, `unload`, `toString` |
| `Material` | `shader` | `setTexture`, `setColor`, `unload`, `toString` |

`Shader:setColor` normalises a `Color` to `0.0`-`1.0` for the uniform. Setting a
uniform the program does not declare is an error, so a typo cannot pass
silently.

`Material:setTexture(map, texture)` and `Material:setColor(map, color)` address
maps with the `ray.materialMap` constants (`albedo`, `metalness`, `normal`,
`roughness`, `occlusion`, `emission`, `height`, plus the `diffuse` and
`specular` aliases). `Model:setMaterial(index, material)` copies a material into
one of the model's slots, duplicating the map array so the model's unload cannot
free the source material's.

`beginShaderMode(shader)`/`endShaderMode()` wrap the following draws in a custom
program and nest with the other modes. The built-in 3D shapes draw without
normals, so the lighting shader only produces sensible results on models.

## Collision, colour, and transforms

These helpers are pure math and need no window.

| Helper | Meaning |
| --- | --- |
| `checkCollisionRecs(a, b)` | Whether two `Rectangle`s overlap |
| `checkCollisionCircles(centerA, radiusA, centerB, radiusB)` | Whether two circles overlap |
| `checkCollisionPointRec(point, rec)` | Whether a point is inside a rectangle |
| `checkCollisionPointCircle(point, center, radius)` | Whether a point is inside a circle |
| `getCollisionRec(a, b)` | Overlap of two rectangles as a `Rectangle` |
| `worldToScreen(position, camera)` / `screenToWorld(position, camera)` | Convert between world and screen space through a `Camera2D` |
| `getWorldToScreen(position, camera)` | World to screen through a `Camera3D` (window required) |
| `getScreenToWorldRay(position, camera)` | Screen to a world-space `Ray` (window required) |
| `raycastSphere(ray, center, radius)` / `raycastBox(ray, min, max)` | Ray collision tests, no window needed |
| `fade(color, alpha)` | Color with scaled alpha, `alpha` in `0.0`-`1.0` |
| `colorLerp(a, b, factor)` | Color interpolation, `factor` in `0.0`-`1.0` |
| `colorBrightness(color, factor)` | Color brightness, `factor` in `-1.0`-`1.0` |
| `colorTint(color, tint)` | Component-wise color multiply |
| `colorFromHSV(hue, saturation, value)` | Color from HSV (`hue` in degrees) |
| `colorToInt(color)` | Color as `0xRRGGBBAA` |

`fade` takes a `0.0`-`1.0` factor, while `color:withAlpha(a)` takes `0`-`255`.

## Window control

| Helper | Meaning |
| --- | --- |
| `setWindowTitle(title)` | Change the window title |
| `setWindowSize(width, height)` | Resize the window (the platform may clamp small sizes) |
| `toggleFullscreen()` | Toggle fullscreen |
| `screenshot(path)` | Queue a capture of this frame to an image file |

## API

| Little API | Meaning |
| --- | --- |
| `open(width, height, title [, options])` | Open the single process window |
| `close()` | Close the window if open |
| `on("start" \| "update", fn)` | Register lifecycle callbacks |
| `update()` | Run one frame; returns `false` when the window should close |
| `clear(color)` | Queue a background clear for this frame; inside a render target it clears that target |
| `rect(bounds, color)` | Queue a filled rectangle for this frame |
| `rectLines(bounds, color)` | Queue a rectangle outline |
| `circle(center, radius, color)` | Queue a filled circle |
| `circleLines(center, radius, color)` | Queue a circle outline |
| `line(start, end, color)` | Queue a line |
| `lineEx(start, end, thickness, color)` | Queue a thick line |
| `triangle(a, b, c, color)` | Queue a filled triangle |
| `triangleLines(a, b, c, color)` | Queue a triangle outline |
| `polygon(center, sides, radius, rotation, color)` | Queue a filled regular polygon |
| `polygonLines(center, sides, radius, rotation, color)` | Queue a polygon outline |
| `ring(center, innerRadius, outerRadius, color)` | Queue a filled ring |
| `text(str, position, size, color)` | Queue default-font text for this frame |
| `beginMode2D(camera)` / `endMode2D()` | Wrap the following draws in a `Camera2D` |
| `beginMode3D(camera)` / `endMode3D()` | Wrap the following draws in a `Camera3D` |
| `beginTextureMode(target)` / `endTextureMode()` | Render the following draws into a `RenderTexture` |
| `beginShaderMode(shader)` / `endShaderMode()` | Draw the following commands with a custom program |
| `cube(p, size, c)`, `cubeWires(p, size, c)`, `sphere(c, r, color)`, `sphereWires(c, r, rings, slices, color)`, `cylinder(p, rt, rb, h, sides, c)`, `cylinderWires(...)`, `capsule(a, b, r, rings, slices, c)`, `plane(c, size, color)`, `triangle3D(a, b, c, color)`, `line3D(a, b, color)`, `point3D(p, color)`, `boundingBox(min, max, color)`, `grid(slices, spacing)`, `billboard(camera, texture, p, scale, tint)` | Queue 3D shapes for this frame |
| `getWorldToScreen(p, camera)` / `getScreenToWorldRay(p, camera)` | Convert through a `Camera3D` |
| `raycastSphere(ray, center, radius)`, `raycastBox(ray, min, max)` | Ray collision tests |
| `keyPressed(code)` / `keyDown(code)` | Whether a key was pressed / is held |
| `keyUp(code)` / `keyReleased(code)` | Whether a key is up / was released |
| `charPressed()` | Next queued character code, `0` when the queue is empty |
| `mouse()` | Current mouse position as a `Vector2` |
| `mouseDelta()` | Mouse movement since the previous frame |
| `mouseWheel()` | Wheel movement this frame |
| `mousePressed(b)` / `mouseDown(b)` / `mouseReleased(b)` | Mouse button state |
| `gamepadButtonDown(pad, b)` / `gamepadButtonPressed(pad, b)` | Gamepad button state |
| `gamepadAxis(pad, axis)` | Gamepad axis value |
| `windowSize()` | Current screen/canvas size as a `Vector2` (window required) |
| `setFPS(n)` | Cap the frame rate |
| `checkCollisionRecs(a, b)`, `checkCollisionCircles(c1, r1, c2, r2)`, `checkCollisionPointRec(p, r)`, `checkCollisionPointCircle(p, c, r)`, `getCollisionRec(a, b)` | Collision helpers |
| `worldToScreen(p, camera)` / `screenToWorld(p, camera)` | Convert through a `Camera2D` |
| `fade(c, a)`, `colorLerp(a, b, f)`, `colorBrightness(c, f)`, `colorTint(c, t)`, `colorFromHSV(h, s, v)`, `colorToInt(c)` | Colour helpers |
| `setWindowTitle(t)`, `setWindowSize(w, h)`, `toggleFullscreen()`, `screenshot(path)` | Window control |
| `time()` / `fps()` | Seconds since init / current frame rate |
| `traceLog(level)` | Set the raylib log threshold (`ray.log.*`) |
| `loadImage(path)`, `genImageColor(w, h, color)` | Create an `Image` |
| `loadTexture(path)`, `loadTextureFromImage(image)` | Create a `Texture` |
| `loadFont(path, size)`, `defaultFont()` | Create a `Font` |
| `genMeshCube(w, h, l)`, `genMeshSphere(r, rings, slices)`, `genMeshPlane(w, l, resX, resZ)`, `genMeshCylinder(r, h, slices)`, `genMeshTorus(r, size, radSeg, sides)`, `genMeshKnot(r, size, radSeg, sides)` | Create a `Mesh` (window required) |
| `loadModel(path)`, `modelFromMesh(mesh)` | Create a `Model` (window required) |
| `loadRenderTexture(w, h)` | Create a `RenderTexture` (window required) |
| `loadShader(vs, fs)`, `loadShaderFromMemory(vs, fs)`, `lightingShader()` | Create a `Shader` (window required) |
| `loadMaterialDefault()` | Create a `Material` (window required) |
| `Vector2`, `Vector3`, `Color`, `Rectangle`, `Camera2D`, `Camera3D`, `Ray` | Value type constructors |
| `Image`, `Texture`, `Font`, `Mesh`, `Model`, `RenderTexture`, `Shader`, `Material` | Resource types (created by their loaders) |
| `RayCollision` | Result type returned by the raycasts |
| `projection` | Camera projection codes (`perspective`, `orthographic`) |
| `materialMap` | Material map codes (`albedo`/`diffuse`, `metalness`/`specular`, `normal`, …) |
| `colors` | Named raylib palette (`lightgray` … `raywhite`, `blank`) |
| `keys` | Keyboard codes (`a` … `z`, `space`, `escape`, `f1`, `kp0`, `leftShift`, …) |
| `mouseButtons` | Mouse button codes (`left`, `right`, `middle`, `side`, `extra`, `forward`, `back`) |
| `gamepad` | `buttons` and `axes` code tables |
| `log` | Trace log levels (`all` … `none`) |
| `version` | Vendored raylib version string |

One window per process: loading the library into a second VM is rejected,
matching the webui library.
