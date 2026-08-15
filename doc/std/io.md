# io

`io.print(...)` writes each argument separated by spaces, followed by a newline.

`io.write(...)` writes each argument separated by spaces, without adding a newline.

`io.writeLine(...)` writes each argument separated by spaces, followed by a newline.

`io.clock()` returns the current execution time of the program in seconds.

`io.readLine()` reads one line from standard input and returns it without the trailing newline, or `null` at end of input. Lines are read into a 1024-byte host buffer; longer input is returned in successive reads.

`io.readFile(path)` reads a whole file as a string. It raises a runtime error if
the file cannot be opened, sized, or allocated. Little strings are
NUL-terminated, so this is a text API rather than a way to preserve embedded
NUL bytes.

`io.writeFile(path, contents)` writes a whole file and returns `true` when all bytes were written. It raises a runtime error if the path cannot be opened.

`io.appendFile(path, contents)` appends to a file and returns `true` when all bytes were written. It raises a runtime error if the path cannot be opened.

`io.appendLine(path, contents)` appends `contents` followed by a `\n` newline and returns `true` when all bytes were written. It raises a runtime error if the path cannot be opened.
