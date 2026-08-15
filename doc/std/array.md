# array

`array.each(x)` returns an iterator function that returns each element in order.

`array.range([start,] end [, step])` returns an iterator function that produces a sequence of numbers.

`array.len(x)` returns the length of an array.

`array.first(x)` returns the first element of a non-empty array.

`array.last(x)` returns the last element of a non-empty array.

`array.pop(x)` removes and returns the last element of an array.

`array.push(array, element)` adds `element` to the back of `array`.

`array.clear(array)` removes all elements.

`array.insert(array, index, element)` inserts `element` at `index`.

`array.remove(array, index)` cyclically removes the element at `index`. This does not preserve order.

`array.contains(array, value)` returns whether `value` is present.

`array.indexOf(array, value)` returns the first index of `value`, or `-1`.

`array.join(array [, separator])` converts elements to strings and joins them.

`array.reverse(array)` returns a new reversed array.

`array.slice(array, start [, count])` returns a new array containing a portion of the source.
