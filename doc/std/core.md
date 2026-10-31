# core

`pcall(fn [, args...])` calls a callable with the remaining arguments and returns { ok: true value: ... } or { ok: false error: ... } instead of letting runtime errors escape.

- `fn`: callable to invoke
- `args...`: arguments passed to fn

`unpack(array [, start [, end]])` returns the elements of an array from start through end as positional return values.

- `array`: array to read elements from
- `start`: first index to read, defaults to 0
- `end`: last index to read, inclusive, defaults to the last index

`import(path)` loads the module at path from the module cache or from disk and returns its exported value.

- `path`: module path to load
