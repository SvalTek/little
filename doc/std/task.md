# task

> See the [task execution model, shared state and limitations](../guides/tasks.md) guide.

`task.run(callable, state)` runs a Little function on a worker thread with a marshalled state value and returns a promise that resolves to its result or rejects with an error string.

- `callable`: Little function to run in the worker
- `state`: value copied into the worker and passed to callable
