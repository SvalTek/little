# table

Tables can be indexed directly with `t.key` or `t[key]`. The `table` module provides utility functions for dynamic access and inspection.

`table.has(table, key)` returns whether `key` maps to a non-null value.

`table.fetch(table, key [, default])` returns the value for `key`, or `default`/`null` when absent.

`table.put(table, key, value)` writes a value.

`table.remove(table, key)` removes a value by storing `null`.

`table.keys(table)` returns an array of non-null keys. Its order is unspecified
and must not be treated as insertion or sorted order.

`table.values(table)` returns an array of non-null values in the corresponding
unspecified key order.

Note: `get` and `set` are reserved class-accessor words, so this module uses `fetch` and `put` for dot-call ergonomics.
