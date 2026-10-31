# io

`io.print(...)` writes each argument separated by spaces, followed by a newline.

- `...`: values to write

`io.write(...)` writes each argument separated by spaces, without appending a newline.

- `...`: values to write

`io.writeLine(...)` writes each argument separated by spaces, followed by a newline.

- `...`: values to write

`io.clock()` returns the processor time consumed by the program so far, in seconds.

`io.readLine()` reads one line from standard input without its trailing newline, returning null at end of input.

`io.readFile(path)` reads the entire file at path and returns its contents as a string.

- `path`: filesystem path of the file to read

`io.writeFile(path, contents)` writes contents to the file at path, replacing any existing contents, and returns true when every byte was written.

- `path`: filesystem path of the file to write
- `contents`: text to write

`io.appendFile(path, contents)` appends contents to the file at path, creating it if absent, and returns true when every byte was written.

- `path`: filesystem path of the file to append to
- `contents`: text to append

`io.appendLine(path, contents)` appends contents followed by a newline to the file at path and returns true when every byte was written.

- `path`: filesystem path of the file to append to
- `contents`: text to append before the newline
