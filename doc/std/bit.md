# bit

`bit.band(a, b)` returns the bitwise and of two 32-bit integers as a signed 32-bit number.

- `a`: left operand
- `b`: right operand

`bit.bor(a, b)` returns the bitwise or of two 32-bit integers as a signed 32-bit number.

- `a`: left operand
- `b`: right operand

`bit.bxor(a, b)` returns the bitwise exclusive or of two 32-bit integers as a signed 32-bit number.

- `a`: left operand
- `b`: right operand

`bit.bnot(a)` returns the bitwise complement of a 32-bit integer as a signed 32-bit number.

- `a`: value to complement

`bit.shl(a, bits)` shifts a left by bits, discarding shifted-out bits, and returns the signed 32-bit result.

- `a`: value to shift
- `bits`: shift count from 0 to 31

`bit.shr(a, bits)` shifts a right by bits, filling the vacated high bits with zero, and returns the signed 32-bit result.

- `a`: value to shift
- `bits`: shift count from 0 to 31

`bit.test(a, index)` returns true when bit index of a is set.

- `a`: value to inspect
- `index`: bit position from 0 to 31

`bit.set(a, index)` returns a with bit index set to one.

- `a`: value to modify
- `index`: bit position from 0 to 31

`bit.clear(a, index)` returns a with bit index set to zero.

- `a`: value to modify
- `index`: bit position from 0 to 31

`bit.toggle(a, index)` returns a with bit index flipped.

- `a`: value to modify
- `index`: bit position from 0 to 31

`bit.count(a)` returns how many of the 32 bits of a are set.

- `a`: value to count set bits of
