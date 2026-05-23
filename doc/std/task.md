# task

`task.run(source, state)` runs Little source text in a fresh VM on a host thread and returns a promise in the parent VM.

```js
task.run("return state.a + state.b", { a: 2 b: 4 })
    .next(fn(value) {
        io.print(value)
    })
    .catch(fn(reason) {
        io.print(reason)
    })
```

## Semantics

`source` must be a string containing Little source text.

`state` is copied into the worker VM as a global named `state`:

```js
task.run("return state.name", { name: "Ada" })
```

The parent VM and worker VM do not share objects or globals. Mutating `state` inside the worker does not mutate the parent value.

The returned promise resolves with the worker script's first returned value, or `null` if it returns no values. Extra return values are discarded.

The returned promise rejects with an error string when:

* `source` is not a string.
* `state` cannot cross the VM boundary.
* the worker source fails to parse, compile, or run.
* the worker result cannot cross the VM boundary.
* the host thread cannot be created.

## Value Copying

Only serializable Little values can cross the task boundary:

* `null`
* numbers
* booleans
* strings
* arrays, recursively
* tables, recursively

Functions, closures, native functions, promises, classes, instances, pointers, and other VM-owned runtime objects cannot cross the boundary:

```js
task.run("return state", { callback: fn() { return 1 } })
    .catch(fn(reason) {
        io.print(reason)
    })
```

## Worker Environment

Workers run in isolated Little VMs. The standard library is opened in the worker, so modules such as `io`, `math`, `array`, `table`, `string`, and `gc` are available.

The async library is not opened inside workers in v1. Worker source should not rely on `Promise`, timers, `async fn`/`await`, or nested `task.run`.

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

Tasks are for isolated work, not shared-memory threading.

* No parent VM memory is shared with workers.
* Values are copied by serialization, not by reference.
* Worker globals are not merged back into the parent.
* `task.run(fn, state)` is not supported; pass source text.
* Cancellation and timeouts are not implemented in v1.
* Worker stdout/stderr behavior is whatever the opened `io` library provides for that VM and host process.
