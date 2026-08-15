# async

`Promise(fn(resolve, reject) { ... })` creates a promise. Promise methods are `next(fn(value) { ... })`, `catch(fn(reason) { ... })`, and `finally(fn() { ... })`.

`setTimeout(callback, ms)` runs `callback` once after `ms` milliseconds and returns a timer id.

`setInterval(callback, ms)` runs `callback` repeatedly until cleared.

`clearTimeout(id)` and `clearInterval(id)` cancel timers.

`mainloop.run()` drains pending async work from inside a Little script until the VM becomes idle or `mainloop.stop()` is called. It returns the number of poll steps performed.

`mainloop.poll()` advances one async step and returns whether work was performed or remains pending.

`mainloop.stop()` requests that the current script-managed mainloop stop. This is useful for interval-driven scripts that intentionally keep work scheduled until a callback decides they are done.

`task.run(callable, state)` runs a Little function on a host thread with explicit shared task state, returning a promise that resolves to the task return value or rejects with an error string. See [task](task.md) for boundary rules and limitations.
