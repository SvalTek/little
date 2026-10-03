# bit

`bit` provides 32-bit operations and the flag helpers that go with them. The
language has no bitwise operators, so this module is the only way to reach them.

Arguments must be whole numbers in the 32-bit range, and every result is an
exact integer. Results are signed, so `bit.shl(1, 31)` is `-2147483648`.

| Helper | Meaning |
| --- | --- |
| `band(a, b)`, `bor(a, b)`, `bxor(a, b)` | Bitwise and, or, xor |
| `bnot(a)` | Bitwise complement |
| `shl(a, bits)` | Shift left; `bits` is 0-31 |
| `shr(a, bits)` | Logical shift right, filling with zero |
| `test(a, index)` | Whether bit `index` is set |
| `set(a, index)`, `clear(a, index)`, `toggle(a, index)` | Set, clear, or flip a bit |
| `count(a)` | Number of set bits |

```js
bit.band(12, 10)   ; 8
bit.bor(12, 10)    ; 14
bit.bxor(12, 10)   ; 6
bit.bnot(0)        ; -1
bit.shl(1, 4)      ; 16
bit.shr(-1, 31)    ; 1

var flags = 0
flags = bit.set(flags, 0)
flags = bit.set(flags, 2)
bit.test(flags, 2) ; true
bit.count(flags)   ; 2
```
