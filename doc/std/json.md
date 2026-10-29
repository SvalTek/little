# json

`json` is the built-in JSON module. It is opened by `ltstd_open_all(vm)` and can
also be opened individually with `ltstd_open_json(vm)`.

`json.parse(text)` parses a JSON document and returns the corresponding Little
value: `null`, a boolean, a number, a string, an array, or a table. Object keys
become string table keys. Invalid JSON returns `null`.

Parsing supports `\uXXXX` escapes, including surrogate pairs. Lone surrogates
become U+FFFD. Little strings are NUL-terminated, so a `\u0000` escape makes the
document invalid. Nesting is limited to 64 levels.

`json.stringify(value)` serializes `null`, booleans, numbers, strings, arrays,
and tables. Non-finite numbers become `null`. Values that JSON cannot represent,
such as functions, instances, and tables with non-string keys, make it return
`null`.
