# string

String literals support `\n`, `\r`, `\t`, `\"`, and `\\` escapes.

`string.from(x)` converts an argument into a string representation.

`string.concat(...)` concatenates string arguments in order.

`string.len(x)` returns the byte length of `x`.

`string.sub(str, start [, length])` creates a substring. If `length` is omitted, the rest of the string is returned.

`string.format(format, ...)` takes a printf-style format string and a list of arguments to insert.

`string.contains(str, needle)` returns whether `needle` exists in `str`.

`string.startsWith(str, prefix)` and `string.endsWith(str, suffix)` test string boundaries.

`string.indexOf(str, needle [, start])` returns the first matching index, or `-1`.

`string.replace(str, needle, replacement)` replaces all occurrences of `needle`.

`string.split(str, separator)` returns an array of parts.

`string.trim(str)`, `string.ltrim(str)`, and `string.rtrim(str)` remove whitespace.

`string.lower(str)` and `string.upper(str)` change ASCII case.

`string.repeat(str, count)` repeats a string.

`string.expand(str, values)` replaces placeholders using a table or array. `{{name}}` and `$name` look up string keys in a table. `$1`, `$2`, and so on look up one-based positions in an array.
