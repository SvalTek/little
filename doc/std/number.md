# number

`number.from(x)` converts a number, or a decimal, 0x hex or 0b binary string, to a number, returning null when the text is not one.

`number.format(value, decimals [, separators])` formats a number with a fixed number of decimals, optionally grouping thousands with commas.

- `decimals`: number of digits after the decimal point, from 0 to 32
- `separators`: whether to insert thousands separators

`number.toBase(value, base [, prefix])` renders a whole number in a base from 2 to 36, with a 0x or 0b prefix when requested.

- `base`: radix to write the number in, from 2 to 36
- `prefix`: whether to prefix a hexadecimal or binary result

`number.isInteger(x)` returns true when the value is finite and has no fractional part.

`number.isSafeInteger(x)` returns true when the value is a whole number that can be represented exactly.

`number.isClose(a, b [, epsilon])` returns true when two numbers differ by no more than a tolerance that scales with their magnitude.

- `epsilon`: relative tolerance, defaulting to 1e-9

`number.trunc(x)` returns the value with its fractional part dropped, towards zero.

`number.roundTo(x, decimals)` returns the value rounded to the given number of decimals.

- `decimals`: number of digits to keep after the decimal point, from 0 to 15

`number.snap(x, step)` returns the value rounded to the nearest multiple of step.

`number.wrap(x, min, max)` wraps the value into the range from min up to but not including max.

`number.pingPong(x, length)` bounces the value back and forth within the range from 0 to length.

`number.map(x, inMin, inMax, outMin, outMax)` rescales the value from the input range onto the output range, without clamping.
