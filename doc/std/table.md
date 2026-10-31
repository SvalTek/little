# table

`table.has(table, key)` reports whether a table holds a non-null value for the given key.

- `table`: table to inspect
- `key`: key to look up

`table.fetch(table, key [, fallback])` returns the value stored under a key, or the fallback when the key is absent.

- `table`: table to read
- `key`: key to look up
- `fallback`: value returned when the key is missing; null if omitted

`table.put(table, key, value)` stores a value under a key and returns nothing.

`table.remove(table, key)` clears the entry for a key and returns nothing.

`table.keys(table)` returns an array of the table's keys, skipping keys whose value is null.

`table.values(table)` returns an array of the table's values, skipping null entries.
