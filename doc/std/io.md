# io

`io.print(...)` writes each argument separated by spaces, followed by a newline.

`io.write(...)` writes each argument separated by spaces, without adding a newline.

`io.clock()` returns the current execution time of the program in seconds.

`io.readLine()` reads one line from standard input and returns it without the trailing newline, or `null` at end of input.

`io.readFile(path)` reads a whole file as a string.

`io.writeFile(path, contents)` writes a whole file and returns `true` when all bytes were written.

`io.appendFile(path, contents)` appends to a file and returns `true` when all bytes were written.
