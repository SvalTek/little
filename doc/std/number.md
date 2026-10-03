# number

`number` is about the number type itself: coercion, formatting, predicates,
ranges, and limits. Arithmetic lives in [math](math.md), and the two do not
overlap.

## Coercion

`number.from(x)` converts to a number, or returns `null` when it cannot. It
accepts a number (returned unchanged) and a string holding any form a literal
accepts: decimal with an optional fraction and exponent, `0x` hex, and `0b`
binary, with optional surrounding whitespace and sign.

```js
number.from("42")      ; 42
number.from("  3.5  ") ; 3.5
number.from("1e3")     ; 1000
number.from("0xff")    ; 255
number.from("0b1010")  ; 10
number.from("-2.5")    ; -2.5
number.from("42x")     ; null
number.from(true)      ; null
```

It never raises: anything that is not a number returns `null`, so a script can
test a string without trapping.

`number.toBase(value, base [, prefix])` renders a whole number in a base from 2
to 36. With `prefix` true, base 16 and base 2 emit `0x` and `0b`, so the result
reads back through `number.from`.

```js
number.toBase(255, 16, true)   ; "0xff"
number.toBase(10, 2, true)     ; "0b1010"
```

## Formatting

`string.from(x)` is the plain coercion. `number.format(value, decimals
[, separators])` is the precision-controlled one, and with `separators` true it
groups thousands.

```js
string.from(42)                  ; "42.000000"
number.format(3.14159, 2)        ; "3.14"
number.format(42, 0)             ; "42"
number.format(1234567, 0, true)  ; "1,234,567"
number.format(-1234.5, 2, true)  ; "-1,234.50"
```

## Predicates

`number.isInteger(x)` is true for whole numbers. `number.isSafeInteger(x)` is
true when the value is a whole number inside the range doubles hold exactly.
`number.isClose(a, b [, epsilon])` compares with a tolerance, which is what
float equality actually needs; the default epsilon is `1e-9` and the tolerance
scales with the larger operand.

```js
number.isInteger(3)                      ; true
number.isInteger(3.5)                    ; false
number.isSafeInteger(9007199254740992)   ; false
number.isClose(0.1 + 0.2, 0.3)           ; true
```

## Ranges and rounding

| Helper | Meaning |
| --- | --- |
| `trunc(x)` | Round towards zero (`floor` and `ceil` cannot express this for negatives) |
| `roundTo(x, decimals)` | Round to a number of decimal places |
| `snap(x, step)` | Round to the nearest multiple of `step` |
| `wrap(x, min, max)` | Wrap into `[min, max)`, so `wrap(-1, 0, 10)` is `9` |
| `pingPong(x, length)` | Bounce between `0` and `length` |
| `map(x, inMin, inMax, outMin, outMax)` | Remap from one range to another |

`clamp` and `lerp` live in [math](math.md).

## Constants

| Name | Meaning |
| --- | --- |
| `epsilon` | Smallest representable difference (`DBL_EPSILON`) |
| `infinity` | Positive infinity |
| `maxSafeInteger` / `minSafeInteger` | ±(2^53 - 1), the largest exactly representable integers |

There is deliberately no `number.nan`. The value representation tags
non-numbers with the quiet-NaN bit pattern, so a real NaN cannot be stored: an
expression that produces one has the type `unknown`. Infinity is fine, because
it keeps a zero mantissa.
