# core

`pcall(fn [, args...])` calls `fn` with optional arguments and returns a result table instead of letting runtime errors escape.

On success:

```js
{ ok: true value: result }
```

On failure:

```js
{ ok: false error: "message" }
```

Trapped errors are not sent to the host error callback.
