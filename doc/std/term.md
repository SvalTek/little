# term

`term` is the CLI's built-in terminal module. It is linked into `little.exe`
using the same ABI as a native library, so its API is deliberately small and
explicit. It is available to CLI scripts; embedders opt in with
`ltstd_open_term(vm)`.

Call `term.open()` before using terminal functions, and `term.close()` when the
program is finished. Opening a session switches the terminal to cbreak and
no-echo input; closing it always restores the normal terminal state.

## Drawing

`term.size()` returns `{ width, height }`. Coordinates passed to `term.move(x,
y)` and `term.write(x, y, text)` are zero-based.

`term.clear()` clears the whole terminal. `term.clearLine(y)` clears one row.
`term.present()` flushes drawing changes. `term.cursor(visible)` shows or hides
the cursor, and `term.bell()` requests the terminal bell.

## Input

`term.poll()` returns one pending input event or `null` without waiting. Event
tables have a numeric `code` and are one of:

- `{ type: "text", text, code }` for printable input;
- `{ type: "key", key, code }` for named keys such as `left`, `right`, `up`,
  `down`, `enter`, `escape`, `backspace`, `delete`, `home`, `end`, `ctrl-c`,
  and `ctrl-d`;
- `{ type: "resize", code }` after a terminal resize.

`term.update()` processes one event using the registered handler and returns
whether it processed one. `term.onEvent(callback)` registers that handler.
While a handler is registered, `mainloop.run()` remains active and dispatches at
most one terminal event each turn, alongside timers and other async work.

`term.readLine(prompt)` is the blocking convenience API. It provides basic line
editing, cursor movement, delete/backspace, and in-process history. It returns
the entered string, or `null` for Ctrl-C, Ctrl-D, or end-of-input. Input is
limited to 4095 characters; further printable characters are rejected with a
terminal bell (`beep()`).

Terminal drawing and `readLine` are intended for an interactive terminal. Do
not call `readLine` from an event callback: it blocks the main loop until the
line is complete.

## CLI prompt

`little -i` keeps one terminal session open: script output scrolls in the area
above its `>> ` bottom prompt line. Enter `"""` alone to begin multiline capture.
Enter `"""` alone to end capture and submit the accumulated chunk. The final
Enter that submits that closing line is the explicit run action. While capture
is active, its submitted lines remain visible and grow upward from the bottom;
the output area above remains intact. On submission, the composer clears and
only the chunk's resulting output is shown, just as for a single-line entry.

Set `repl_echo = true` in `little.conf` to echo submitted input into the output
transcript. It applies equally to one-line entries and multiline captures; the
default is `false`.
