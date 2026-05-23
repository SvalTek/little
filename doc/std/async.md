# async

`Promise(fn(resolve, reject) { ... })` creates a promise. Promise methods are `next(fn(value) { ... })`, `catch(fn(reason) { ... })`, and `finally(fn() { ... })`.

`setTimeout(callback, ms)` runs `callback` once after `ms` milliseconds and returns a timer id.

`setInterval(callback, ms)` runs `callback` repeatedly until cleared.

`clearTimeout(id)` and `clearInterval(id)` cancel timers.

`task.run(source, state)` runs Little `source` in an isolated VM with copied global `state`, returning a promise that resolves to the task return value or rejects with an error string.
