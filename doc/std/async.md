# async

> See the [promises, timers and the mainloop](../guides/async.md) guide.

`Promise(executor)` creates a promise and calls executor immediately with resolve and reject callbacks.

- `executor`: function called with resolve and reject callbacks

`setTimeout(callback, ms)` runs callback once after ms milliseconds and returns a timer id.

- `callback`: function invoked once after the delay
- `ms`: delay in milliseconds

`setInterval(callback, ms)` runs callback every ms milliseconds until the timer is cancelled and returns a timer id.

- `callback`: function invoked on each tick
- `ms`: interval in milliseconds

`clearTimeout(id)` cancels the timer with the given id.

- `id`: timer id returned by setTimeout or setInterval

`clearInterval(id)` cancels the timer with the given id.

- `id`: timer id returned by setInterval or setTimeout

`mainloop.run()` drains pending async work until the VM is idle or mainloop.stop is called and returns the number of poll steps performed.

`mainloop.poll()` advances one async step and returns whether work was performed or remains pending.

`mainloop.stop()` requests that the current script-managed mainloop stop.
