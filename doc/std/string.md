# string

`string.from(value)` converts any value to its string representation.

`string.concat(str1, str2, ...)` joins two or more strings into one, in argument order.

- `str1`: first string to join
- `str2`: second string to join

`string.len(str)` returns the number of bytes in a string.

- `str`: string to measure

`string.sub(str, start [, length])` returns the substring starting at a zero-based offset, running to the end of the string or for a given length.

- `str`: string to slice
- `start`: zero-based offset of the first byte
- `length`: number of bytes to copy; the rest of the string if omitted

`string.byte(str, index)` returns the numeric value of the byte at a zero-based index, or null when the index is outside the string.

- `index`: zero-based byte offset, as a whole number

`string.char(code)` returns a one-byte string holding the character for a numeric code between 1 and 255.

- `code`: whole number code between 1 and 255

`string.format(template, ...)` builds a string from a printf-style template, converting %d, %i, %o, %u, %x, %X, %e, %E, %f, %g, %G and %s specifiers, with %% producing a literal percent sign and output capped at 1023 bytes.

- `template`: format string with conversion specifiers
- `...`: one value per conversion specifier, of the matching type

`string.contains(str, needle)` reports whether a string occurs anywhere inside another.

`string.startsWith(str, prefix)` reports whether a string begins with the given prefix.

`string.endsWith(str, suffix)` reports whether a string ends with the given suffix.

`string.indexOf(str, needle [, start])` returns the zero-based position of the first occurrence of a needle at or after an offset, or -1 when it is not found.

- `start`: zero-based offset to begin searching from; defaults to 0

`string.replace(str, needle, replacement)` returns a copy of a string with every occurrence of a needle replaced; an empty needle leaves the string unchanged.

`string.split(str, separator)` splits a string on every occurrence of a separator and returns the parts as an array, splitting into single characters when the separator is empty.

`string.trim(str)` returns a copy of a string with whitespace removed from both ends.

`string.ltrim(str)` returns a copy of a string with leading whitespace removed.

`string.rtrim(str)` returns a copy of a string with trailing whitespace removed.

`string.lower(str)` returns a copy of a string with every ASCII letter converted to lower case.

`string.upper(str)` returns a copy of a string with every ASCII letter converted to upper case.

`string.repeat(str, count)` returns a string consisting of the source repeated a non-negative whole number of times.

- `count`: number of repetitions

`string.expand(template, values)` substitutes {{key}} and $name placeholders in a template with values looked up in a table by key or in an array by one-based index, rendering unknown keys as the text null.

- `template`: text containing placeholders
- `values`: table keyed by placeholder name, or array indexed from 1
