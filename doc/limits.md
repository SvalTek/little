# Little limits

Little is intentionally small and uses fixed-size VM stacks for predictable embedding. These limits are defaults from `little.h`; embedders may override the `LT_*` defines before including/building Little, but larger limits increase VM memory usage and should be tested.

## VM limits
| Limit | Default | Notes |
| --- | ---: | --- |
| `LT_STACK_SIZE` | 256 | Value stack entries. This stack holds operands, locals, arguments, and return values. Overflow now fails with `VM stack overflow!`. |
| `LT_CALLSTACK_SIZE` | 32 | Active Little call frames. Recursive calls beyond this fail with `Call stack overflow!`. |
| `LT_DEDUP_TABLE_SIZE` | 64 | String dedup hash buckets. This is not a string-count limit; buckets grow dynamically. |

## Parser/compiler limits
| Limit | Default | Practical source limit |
| --- | ---: | ---: |
| `LT_MAX_FUNCTION_PARAMS` | 16 | 16 declared parameters. |
| `LT_MAX_CALL_ARGS` | 16 | 16 call arguments. Method-call sugar passes the receiver internally, so `obj:method(...)` supports 15 user arguments by default. |
| `LT_MAX_BRANCHES` | 32 | 32 compiled branch exits in one `if` / `elseif` chain. Extremely long chains fail with `Too many if/elseif branches!`. |
| `LT_MAX_RETURNS` | 255 | 255 returned values from one call. |
| `LT_MAX_CONSTANTS` | 32767 | Constants per compiled function/chunk. |
| `LT_MAX_LOCALS` | `LT_STACK_SIZE - 1` | Locals per compiled function/chunk, also constrained by the VM value stack. |

Function and call argument overflow is a parse error. Branch-chain overflow is a compile error. Both are intended to fail cleanly rather than corrupt parser/compiler memory.

## Multi-return limits
Multi-return counts are stored in `uint8_t`, so one call can produce at most `LT_MAX_RETURNS` returned values. `unpack` stops at that limit. Scalar contexts collapse multi-return values to the first value, or `null` if no values were returned.

## Loops and execution time
`while` and `for` loops do not have an instruction budget or iteration cap. A script can intentionally run forever:
```js
while true {
}
```
Embedders that need time limits should run Little in a supervised host environment or add an instruction budget to the VM.

## Host recursion
Parser nesting and expression complexity do not currently have a dedicated source-level depth limit. Very deeply nested source can still exhaust host C stack or memory before reaching a Little-level error. The runtime call stack is guarded separately by `LT_CALLSTACK_SIZE`.
