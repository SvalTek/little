#include "little_std.h"

#include <math.h>
#include <stdio.h>

/* Bit operations are 32-bit and signed, matching what the double-backed number
   type holds exactly, so every result is an exact integer. The language has no
   bitwise operators, so this module is the only way to reach them; the flag
   helpers cover the packed-value work that goes with them. */

static uint32_t _lt_bit_pop(lt_VM* vm, const char* name)
{
    char message[96];
    snprintf(message, sizeof(message), "Expected argument to bit.%s to be number!", name);
    lt_Value value = lt_pop(vm);
    if (!LT_IS_NUMBER(value)) lt_runtime_error(vm, message);
    double number = LT_GET_NUMBER(value);
    /* The bound is asymmetric on purpose: -2147483648 is a valid int32_t and is
       what shl(1, 31) returns, while +2147483648 is not representable and
       converting it is undefined. */
    if (!isfinite(number) || number != floor(number) || number > 2147483647.0 || number < -2147483648.0)
        lt_runtime_error(vm, "Expected bit operation argument to be a 32-bit whole number!");
    return (uint32_t)(int32_t)number;
}

/* Bit indices are validated separately because a negative index arrives here as
   a very large unsigned value and would otherwise shift out of range. */
static uint32_t _lt_bit_index(lt_VM* vm, const char* name)
{
    uint32_t index = _lt_bit_pop(vm, name);
    if (index > 31)
    {
        char message[96];
        snprintf(message, sizeof(message), "Expected bit.%s index between 0 and 31!", name);
        lt_runtime_error(vm, message);
    }
    return index;
}

#define LT_BIT_BINARY(name, op) \
    static uint8_t _lt_bit_##name(lt_VM* vm, uint8_t argc) \
    { \
        if (argc != 2) lt_runtime_error(vm, "Expected two arguments to bit." #name "!"); \
        uint32_t b = _lt_bit_pop(vm, #name); \
        uint32_t a = _lt_bit_pop(vm, #name); \
        lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(a op b))); \
        return 1; \
    }

LT_BIT_BINARY(band, &);
LT_BIT_BINARY(bor, |);
LT_BIT_BINARY(bxor, ^);

static uint8_t _lt_bit_bnot(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to bit.bnot!");
    lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(~_lt_bit_pop(vm, "bnot"))));
    return 1;
}

static uint8_t _lt_bit_shl(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a value and a shift to bit.shl!");
    uint32_t shift = _lt_bit_index(vm, "shl");
    uint32_t value = _lt_bit_pop(vm, "shl");
    lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(value << shift)));
    return 1;
}

/* Logical shift: the vacated high bits fill with zero, which is what a script
   unpacking data wants. The result is cast back to signed like every other
   helper, so shr(-1, 0) is -1 rather than 4294967295. */
static uint8_t _lt_bit_shr(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a value and a shift to bit.shr!");
    uint32_t shift = _lt_bit_index(vm, "shr");
    uint32_t value = _lt_bit_pop(vm, "shr");
    lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(value >> shift)));
    return 1;
}

static uint8_t _lt_bit_test(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a value and an index to bit.test!");
    uint32_t index = _lt_bit_index(vm, "test");
    uint32_t value = _lt_bit_pop(vm, "test");
    lt_push(vm, ((value >> index) & 1u) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_bit_set(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a value and an index to bit.set!");
    uint32_t index = _lt_bit_index(vm, "set");
    uint32_t value = _lt_bit_pop(vm, "set");
    lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(value | (1u << index))));
    return 1;
}

static uint8_t _lt_bit_clear(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a value and an index to bit.clear!");
    uint32_t index = _lt_bit_index(vm, "clear");
    uint32_t value = _lt_bit_pop(vm, "clear");
    lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(value & ~(1u << index))));
    return 1;
}

static uint8_t _lt_bit_toggle(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a value and an index to bit.toggle!");
    uint32_t index = _lt_bit_index(vm, "toggle");
    uint32_t value = _lt_bit_pop(vm, "toggle");
    lt_push(vm, LT_VALUE_NUMBER((double)(int32_t)(value ^ (1u << index))));
    return 1;
}

static uint8_t _lt_bit_count(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to bit.count!");
    uint32_t value = _lt_bit_pop(vm, "count");
    uint32_t count = 0;
    while (value)
    {
        count += value & 1u;
        value >>= 1;
    }
    lt_push(vm, LT_VALUE_NUMBER((double)count));
    return 1;
}

void ltstd_open_bit(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);

    lt_table_set(vm, t, lt_make_string(vm, "band"), lt_make_native(vm, _lt_bit_band));
    lt_table_set(vm, t, lt_make_string(vm, "bor"), lt_make_native(vm, _lt_bit_bor));
    lt_table_set(vm, t, lt_make_string(vm, "bxor"), lt_make_native(vm, _lt_bit_bxor));
    lt_table_set(vm, t, lt_make_string(vm, "bnot"), lt_make_native(vm, _lt_bit_bnot));
    lt_table_set(vm, t, lt_make_string(vm, "shl"), lt_make_native(vm, _lt_bit_shl));
    lt_table_set(vm, t, lt_make_string(vm, "shr"), lt_make_native(vm, _lt_bit_shr));

    lt_table_set(vm, t, lt_make_string(vm, "test"), lt_make_native(vm, _lt_bit_test));
    lt_table_set(vm, t, lt_make_string(vm, "set"), lt_make_native(vm, _lt_bit_set));
    lt_table_set(vm, t, lt_make_string(vm, "clear"), lt_make_native(vm, _lt_bit_clear));
    lt_table_set(vm, t, lt_make_string(vm, "toggle"), lt_make_native(vm, _lt_bit_toggle));
    lt_table_set(vm, t, lt_make_string(vm, "count"), lt_make_native(vm, _lt_bit_count));

    lt_table_set(vm, vm->global, lt_make_string(vm, "bit"), t);
}
