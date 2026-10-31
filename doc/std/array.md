# array

`array.each(array)` returns an iterator function that yields each element of array in order.

`array.range([start,] end [, step])` returns an iterator producing numbers from start up to but excluding end, advancing by step.

- `start`: first value, defaults to 0
- `step`: increment between values, defaults to 1 and must be non-zero

`array.len(array)` returns the number of elements in array.

`array.first(array)` returns the first element of a non-empty array.

`array.last(array)` returns the last element of a non-empty array.

`array.pop(array)` removes and returns the last element of array.

`array.push(array, element)` appends element to the back of array.

`array.clear(array)` removes every element from array.

`array.insert(array, index, element)` inserts element at index, shifting later elements right; an index past the end appends.

- `index`: non-negative integer position to insert at

`array.remove(array, index)` removes the element at index, moving the last element into its place so order is not preserved.

- `index`: integer position of the element to remove

`array.contains(array, value)` returns true when value is present in array.

- `value`: value to search for

`array.indexOf(array, value)` returns the index of the first occurrence of value in array, or -1 when absent.

- `value`: value to search for

`array.join(array [, separator])` returns the elements converted to strings and concatenated with separator between them.

- `separator`: string inserted between elements, defaults to the empty string

`array.reverse(array)` returns a new array holding the elements of array in reverse order.

`array.slice(array, start [, count])` returns a new array of count elements from array starting at start, defaulting to the rest of the array.

- `start`: index of the first element to copy
- `count`: number of elements to copy
