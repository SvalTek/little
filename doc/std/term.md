# term

`term.open()` opens the terminal for the calling VM.

`term.close()` closes and releases the terminal owned by the calling VM.

`term.isOpen()` returns true when the calling VM owns the active terminal.

`term.size()` returns a table with the terminal's width and height in cells.

`term.clear()` clears the screen and resets the output position to the origin.

`term.clearLine(y)` clears the terminal row at the given screen row.

- `y`: zero-based screen row to clear

`term.present()` flushes pending output to the terminal display.

`term.move(x, y)` moves the cursor to the given column and row.

- `x`: zero-based column
- `y`: zero-based row

`term.cursor(visible)` shows or hides the terminal cursor.

- `visible`: true to show the cursor, false to hide it

`term.write(x, y, text)` writes text at the given column and row.

- `x`: zero-based column
- `y`: zero-based row
- `text`: string to display

`term.bell()` emits the terminal alert sound.

`term.poll()` returns the next available input event, or null when no key is pending.

`term.update()` processes one pending terminal event and returns true when work was performed.

`term.onEvent(callback)` registers a callable invoked with each terminal event, replacing any previous callback, and returns it.

- `callback`: function receiving an event table with type, code, and key or text

`term.readLine(prompt)` reads an editable line of input after the prompt, returning null when input is cancelled.

- `prompt`: text displayed before the input line
