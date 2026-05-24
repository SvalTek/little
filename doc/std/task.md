# task

`task.run(callable, state)` runs a Little function in a fresh VM on a host thread and returns a promise in the parent VM.

```js
task.run(fn(state) {
    return state.a + state.b
}, { a: 2 b: 4 })
    .next(fn(value) {
        io.print(value)
    })
    .catch(fn(reason) {
        io.print(reason)
    })
```

## Semantics

`callable` must be a Little function. Source strings are not accepted.

`state` is the explicit task boundary. The worker receives it as argument `state`:

```js
task.run(fn(state) {
    return state.name
}, { name: "Ada" })
```

Closure captures are not imported in v1. Pass values through `state` instead.

The returned promise resolves with the callable's first returned value, or `null` if it returns no values. Extra return values are discarded.

The returned promise rejects with an error string when:

* `state` cannot cross the VM boundary.
* the callable captures upvalues.
* the callable or worker async work fails at runtime.
* the worker result cannot cross the VM boundary.
* the host thread cannot be created.

## Shared State

Tables and arrays in `state` are promoted to shared task objects. The parent VM and worker VM each hold proxy objects to the same backing storage, so mutations through table fields and array elements are visible across the boundary:

```js
var box = { value: 1 }
var state = { x: box }

task.run(fn(state) {
    state.x.value = state.x.value + 1
}, state)
```

Changing `state.x.value` in the worker updates the shared `box` table, so `box.value` becomes 2 in the parent VM as well.

Scalar values are copied. Little does not infer variable origins:

```js
var foo = 1
var state = { x: foo }
```

Changing `state.x` in the worker updates the shared `state` table, but it does not rebind `foo`.

Unsupported state values include functions, closures, native functions, promises, classes, instances, pointers, and other VM-owned runtime objects.

Shared task objects are collected by a small shared-graph collector. VM proxies and in-flight worker results are roots; cycles inside shared task state do not rely on reference counts to break themselves.

To keep malformed or hostile graphs bounded, task state import rejects graphs deeper than `LT_TASK_SHARED_MAX_DEPTH` and graphs with more than `LT_TASK_SHARED_MAX_NODES` shared table/array objects. The defaults are 256 levels and 4096 objects.

## Worker Environment

Workers run in isolated Little VMs with the standard library, Promise, timers, `async fn`, and `await` available.

Nested `task.run` is intentionally unavailable inside workers.

## Scheduling

Worker completion is delivered back to the parent VM through the event loop. The CLI calls `lt_runloop(vm)` after top-level script execution, so pending tasks are drained before the process exits.

Embedded hosts that use `task.run` must continue polling or draining the VM:

```c
while (lt_poll(vm)) {
    /* host work */
}
```

or call:

```c
lt_runloop(vm);
```

## Limitations

* Shared task state currently supports tables and arrays as reference-like values.
* Scalar variable rebinding does not cross the task boundary.
* Closure captures are rejected; use explicit `state`.
* Classes, instances, promises, pointers, and functions cannot be placed in shared state.
* Cancellation and timeouts are not implemented in v1.
* Worker stdout/stderr behavior is whatever the opened `io` library provides for that VM and host process.
