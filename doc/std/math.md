# math

Simple wrappers around `math.h`:

`math.sin(x)`, `math.cos(x)`, `math.tan(x)`, `math.asin(x)`, `math.acos(x)`, `math.atan(x)`, `math.sinh(x)`, `math.cosh(x)`, `math.tanh(x)`, `math.floor(x)`, `math.ceil(x)`, `math.round(x)`, `math.exp(x)`, `math.log(x)`, `math.log10(x)`, `math.sqrt(x)`, `math.abs(x)`, `math.min(a, b)`, `math.max(a, b)`, `math.pow(a, b)`, and `math.mod(a, b)`.

Utility helpers:

`math.clamp(x, min, max)` clamps `x` into the inclusive range.

`math.lerp(a, b, t)` linearly interpolates from `a` to `b`.

`math.sign(x)` returns `-1`, `0`, or `1`.

`math.isnan(x)` and `math.isfinite(x)` test number state.

`math.deg(radians)` converts radians to degrees.

`math.rad(degrees)` converts degrees to radians.

`math.random()` returns a number in `[0, 1)`.

`math.randomInt(min, max)` returns an integer in the inclusive range.

`math.seed(n)` seeds the random number generator.

Constants: `math.pi`, `math.e`.
