# Promises, timers and the mainloop

The signatures for `Promise`, `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` and the `mainloop` helpers live in [std/async.md](../std/async.md); this guide covers how they run.

## Promises

`Promise(fn(resolve, reject) { ... })` calls the executor immediately, before the constructor returns. `resolve(value)` and `reject(reason)` settle the promise; a promise settles once.

`next(fn(value) { ... })`, `catch(fn(reason) { ... })` and `finally(fn() { ... })` register reactions. Reactions do not run inline: they are queued and drained by the event loop, so code after the `.next(...)` call runs before the reaction does.

An `async fn` returns a promise. `await` polls the event loop from the current executing frame until the value settles, then continues with the result. It does not create a suspended coroutine. If the awaited value is not a promise, `await` returns it unchanged.

Callable results follow Little's multi-return rule at promise and task boundaries: no values become `null`, one value stays scalar, and multiple values become an array. Destructure the awaited array with `var [a, b] = await work()`. Runtime errors inside an async function or promise callback reject the resulting promise.

`pcall` only catches errors raised while its callback is executing. It does not catch a later rejection of a promise the callback returned; attach `.catch(...)` to observe that.

## Timers

`setTimeout(callback, ms)` schedules one call after the delay and returns a timer id. `setInterval(callback, ms)` schedules a repeating call and returns a timer id. `clearTimeout(id)` and `clearInterval(id)` cancel either kind of timer.

Timers are host-side schedules: the callback runs when the event loop is polled after the delay has elapsed, not preemptively. A long-running Little loop that never polls delays every timer.

## The mainloop

`mainloop.poll()` advances one step of queued work and reports whether work was performed or remains pending. It never waits.

`mainloop.run()` drains pending work from inside a Little script until the VM is idle or `mainloop.stop()` is called, and returns the number of poll steps performed. This is useful when a script manages its own loop, for example an interval-driven program that keeps running until a callback decides it is done.

`mainloop.stop()` requests that the current script-managed mainloop stop. It does not cancel timers or promises that are already queued.

Outside a script, the CLI calls `lt_runloop(vm)` after top-level execution so pending work is drained before the process exits. Embedded hosts using timers, promises or `task.run` must keep polling (`while (lt_poll(vm)) { ... }`) or call `lt_runloop(vm)`.
