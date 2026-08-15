# Little WebUI native library

This optional native library wraps a small, callback-capable subset of
[WebUI](https://github.com/webui-dev/webui).

It is not part of Little's core language or standard library. Scripts opt into
it explicitly with `loadLibrary(...)`, and embedders can omit `loadLibrary`
support entirely if native library loading should be blocked.

## Build

Build it after cloning WebUI into `vendor/webui`:

```powershell
task build:nativelibs
```

The build creates `nativelib/webui/build/webui.dll` on Windows, or the matching
platform shared-library extension on other systems.

## Load

```js
var webui = loadLibrary("nativelib/webui/build/webui")
```

The module value is a table of native functions and constants. The library uses
WebUI's browser-window mode by default; `showWv` is exposed for WebView mode.
On macOS, the native build includes WebUI's WKWebView backend and links Cocoa
and WebKit.

## Basic Window

Example:

```js
var webui = loadLibrary("nativelib/webui/build/webui")

var win = webui.newWindow()
webui.setRootFolder(win, "path/to/ui")

webui.bind(win, "hello", fn(event) {
    io.print("hello", event.payload.name)

    return Promise(fn(resolve, reject) {
        setTimeout(fn() {
            resolve({ message: "Hello from Little" })
        }, 10)
    })
})

webui.show(win, """
<html>
<head><script src="webui.js"></script></head>
<body>
    <script>
    function sendToLittle(name, payload) {
        return webui.call(name, JSON.stringify(payload)).then((response) => JSON.parse(response))
    }
    </script>
    <button onclick="sendToLittle('hello', { name: 'Ada' }).then((response) => document.body.insertAdjacentHTML('beforeend', '<p>' + response.message + '</p>'))">Hello</button>
</body>
</html>
""")

webui.wait()
webui.clean()
```

`webui.js` must be included by pages that need to call back into Little:

```html
<script src="/webui.js"></script>
```

For real applications, prefer static UI files:

```js
webui.setRootFolder(win, "ui")
webui.show(win, "index.html")
```

`setRootFolder(window, path)` and `setDefaultRootFolder(path)` must be called
before `show(...)` for the relevant window.

## Callbacks

Bind a named JavaScript event/function to a Little function:

```js
webui.bind(win, "ready", fn(event) {
    io.print(event.element)
    io.print(event.payload.name)

    return { message: "handled" }
})
```

Browser code usually sends one JSON payload string:

```html
<script>
function sendToLittle(name, payload) {
    return webui.call(name, JSON.stringify(payload))
        .then((response) => JSON.parse(response))
}
</script>
```

The native library parses that first JSON string and exposes it as
`event.payload`. It also exposes the raw WebUI argument data:

| Field | Meaning |
| --- | --- |
| `event.window` | WebUI window id |
| `event.eventNumber` | WebUI event number |
| `event.eventType` | WebUI event type |
| `event.clientId` | WebUI client id |
| `event.connectionId` | WebUI connection id |
| `event.element` | Bound element/function name |
| `event.cookies` | WebUI cookie string |
| `event.args` | Array of raw argument tables |
| `event.first` | First raw string argument, or `null` |
| `event.payload` | Parsed JSON payload when there is exactly one argument |

Each raw argument table contains `string`, `int`, `float`, and `bool` views.

Bind `"*"` to receive WebUI lifecycle events:

```js
webui.bind(win, "*", fn(event) {
    io.print("webui event:", event.eventType)
})
```

## Responses

Little callback return values are serialized back to JavaScript as JSON response
strings. JavaScript should parse the response before using it as an object.

Callbacks may return a `Promise`; the WebUI library polls Little async work
until the promise settles and then sends the fulfilled value as the response:

```js
webui.bind(win, "load", fn(event) {
    return Promise(fn(resolve, reject) {
        setTimeout(fn() {
            resolve({ ok: true })
        }, 10)
    })
})
```

The WebUI C callback copies event data into a native queue. Little callbacks are
dispatched later by `webui.poll()` or `webui.wait()`, so the Little VM is entered
from the Little-owning thread.

## Runtime Scripts

When serving `.ts` files, choose the WebUI runtime before `show()`:

```js
webui.setRuntime(win, webui.Deno)
; or webui.setRuntime(win, webui.Bun)
; or webui.setRuntime(win, webui.NodeJS)
```

`RuntimeNone` disables runtime interpretation and serves `.js`/`.ts` files as
normal static text.

Important: the runtime applies to every `.js` and `.ts` file served from that
WebUI window's root folder. With `Deno`, `Bun`, or `NodeJS` enabled, a browser
request for `app.js` is treated as a runtime script request, not as static
browser JavaScript. This mirrors WebUI's C API behavior and is useful for
CGI-like data endpoints, but it is surprising if you expect ordinary static JS.

For pages that use runtime-backed `.ts` endpoints, keep browser JavaScript
inline in the HTML or place it somewhere WebUI will not interpret as a served
`.js`/`.ts` file. Then request runtime files explicitly:

```html
<script>
async function loadTotal() {
    const data = await fetch("deno_test.ts?foo=123&bar=456")
        .then((response) => response.json())

    await webui.call("ready", JSON.stringify({ deno: data }))
}
</script>
```

The runtime script should print the HTTP response body to stdout:

```ts
const query = Deno.args[0] ?? ""
const params = new URLSearchParams(query)
const foo = Number(params.get("foo") ?? 0)
const bar = Number(params.get("bar") ?? 0)

console.log(JSON.stringify({
    runtime: "deno",
    total: foo + bar
}))
```

WebUI captures that stdout and returns it as the browser fetch response. The
browser can parse it and send the object back to Little through `webui.call`;
the native WebUI library will expose it as a nested Little table.

The demo under `scripts/webui/` proves the full round trip:

```text
browser fetch -> Deno stdout JSON -> browser JSON object -> webui.call -> Little table
```

## Window Lifecycle

Window lifecycle helpers map to the WebUI C API:

| Little API | Meaning |
| --- | --- |
| `newWindow()` | Create a WebUI window and return its id |
| `show(window, content)` | Show HTML content or a file path |
| `showBrowser(window, content, browser)` | Show using a specific browser constant |
| `showWv(window, content)` | Show using WebView mode |
| `wait()` | Process WebUI work and Little callback queue until WebUI stops |
| `poll()` | Process queued callbacks once and return the dispatch count |
| `run(window, script)` | Run JavaScript in the window |
| `close(window)` | Close a window that can be shown again later |
| `closeClient(event)` | Close the client that sent a callback event |
| `destroy(window)` | Close a window and free its WebUI resources |
| `exit()` | Close all WebUI windows and make `wait()` return |
| `clean()` | Free WebUI global resources |
| `isShown(window)` | Return whether a window is shown |
| `focus(window)` | Focus a window |
| `minimize(window)` | Minimize a window |
| `maximize(window)` | Maximize a window |
| `setSize(window, width, height)` | Set window size |
| `setCenter(window)` | Center the window |
| `setRootFolder(window, path)` | Set a window's static root folder |
| `setDefaultRootFolder(path)` | Set the default static root folder |
| `setRuntime(window, runtime)` | Set `.js`/`.ts` runtime interpretation |

Use `closeClient(event)` from inside a callback when you want to close the
browser/client that sent that specific event. Use `close(window)` when you want
the whole window/browser page closed. Calls made while dispatching an event are
deferred until after the callback response is sent, so JavaScript promises can
resolve before the connection is closed.

## Constants

Browser constants:

```text
AnyBrowser, NoBrowser, Chrome, Firefox, Edge, Webview
```

Runtime constants:

```text
RuntimeNone, Deno, NodeJS, Bun
```

## Logging

The WebUI upstream `WEBUI_LOG` build flag is intentionally not enabled for this
library. In WebUI it also enables verbose logging inside allocator, websocket,
server, and close paths, which can perturb shutdown behavior. If wrapper-level
logging is needed later, it should be implemented in this native library without
enabling upstream verbose logging.

## Known Scope

This library is intentionally a small optional binding. It does not expose every
WebUI function yet, and it does not integrate with `import`; use `loadLibrary`
for native libraries and `import` for `.little` source modules.

WebUI callback state is process-global. Load this library into only one Little
VM per process; a second VM load is rejected rather than routing callbacks to
the wrong VM.
