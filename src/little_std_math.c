#include "little_std.h"

#include <math.h>

#define LT_SIMPLE_MATH_FN(name) \
    static uint8_t _lt_##name(lt_VM* vm, uint8_t argc) \
{ \
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math." #name "!"); \
    lt_Value arg = lt_pop(vm); \
    if (!LT_IS_NUMBER(arg)) lt_runtime_error(vm, "Expected argument to math." #name " to be number!"); \
    lt_push(vm, LT_VALUE_NUMBER(name(LT_GET_NUMBER(arg)))); \
    return 1; \
}

LT_SIMPLE_MATH_FN(sin);
LT_SIMPLE_MATH_FN(cos);
LT_SIMPLE_MATH_FN(tan);

LT_SIMPLE_MATH_FN(sinh);
LT_SIMPLE_MATH_FN(cosh);
LT_SIMPLE_MATH_FN(tanh);

LT_SIMPLE_MATH_FN(asin);
LT_SIMPLE_MATH_FN(acos);
LT_SIMPLE_MATH_FN(atan);

LT_SIMPLE_MATH_FN(round);
LT_SIMPLE_MATH_FN(ceil);
LT_SIMPLE_MATH_FN(floor);

LT_SIMPLE_MATH_FN(exp);
LT_SIMPLE_MATH_FN(log);
LT_SIMPLE_MATH_FN(log10);
LT_SIMPLE_MATH_FN(sqrt);
LT_SIMPLE_MATH_FN(fabs);

#define LT_BINARY_MATH_FN(name) \
    static uint8_t _lt_##name(lt_VM* vm, uint8_t argc) \
{ \
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to math." #name "!"); \
    lt_Value arg1 = lt_pop(vm); \
    lt_Value arg2 = lt_pop(vm); \
    if (!LT_IS_NUMBER(arg1) || !LT_IS_NUMBER(arg2)) lt_runtime_error(vm, "Expected argument to math." #name " to be number!"); \
    lt_push(vm, LT_VALUE_NUMBER(name(LT_GET_NUMBER(arg1), LT_GET_NUMBER(arg2)))); \
    return 1; \
}

LT_BINARY_MATH_FN(fmin);
LT_BINARY_MATH_FN(fmax);
LT_BINARY_MATH_FN(pow);
LT_BINARY_MATH_FN(fmod);

void ltstd_open_math(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    lt_table_set(vm, t, lt_make_string(vm, "sin"), lt_make_native(vm, _lt_sin));
    lt_table_set(vm, t, lt_make_string(vm, "cos"), lt_make_native(vm, _lt_cos));
    lt_table_set(vm, t, lt_make_string(vm, "tan"), lt_make_native(vm, _lt_tan));

    lt_table_set(vm, t, lt_make_string(vm, "asin"), lt_make_native(vm, _lt_asin));
    lt_table_set(vm, t, lt_make_string(vm, "acos"), lt_make_native(vm, _lt_acos));
    lt_table_set(vm, t, lt_make_string(vm, "atan"), lt_make_native(vm, _lt_atan));

    lt_table_set(vm, t, lt_make_string(vm, "sinh"), lt_make_native(vm, _lt_sinh));
    lt_table_set(vm, t, lt_make_string(vm, "cosh"), lt_make_native(vm, _lt_cosh));
    lt_table_set(vm, t, lt_make_string(vm, "tanh"), lt_make_native(vm, _lt_tanh));

    lt_table_set(vm, t, lt_make_string(vm, "floor"), lt_make_native(vm, _lt_floor));
    lt_table_set(vm, t, lt_make_string(vm, "ceil"),  lt_make_native(vm, _lt_ceil));
    lt_table_set(vm, t, lt_make_string(vm, "round"), lt_make_native(vm, _lt_round));

    lt_table_set(vm, t, lt_make_string(vm, "exp"),   lt_make_native(vm, _lt_exp));
    lt_table_set(vm, t, lt_make_string(vm, "log"),   lt_make_native(vm, _lt_log));
    lt_table_set(vm, t, lt_make_string(vm, "log10"), lt_make_native(vm, _lt_log10));
    lt_table_set(vm, t, lt_make_string(vm, "sqrt"),  lt_make_native(vm, _lt_sqrt));
    lt_table_set(vm, t, lt_make_string(vm, "abs"),   lt_make_native(vm, _lt_fabs));

    lt_table_set(vm, t, lt_make_string(vm, "min"), lt_make_native(vm, _lt_fmin));
    lt_table_set(vm, t, lt_make_string(vm, "max"), lt_make_native(vm, _lt_fmax));
    lt_table_set(vm, t, lt_make_string(vm, "pow"), lt_make_native(vm, _lt_pow));
    lt_table_set(vm, t, lt_make_string(vm, "mod"), lt_make_native(vm, _lt_fmod));

    lt_table_set(vm, t, lt_make_string(vm, "pi"), LT_VALUE_NUMBER(3.14159265358979323846));
    lt_table_set(vm, t, lt_make_string(vm, "e"), LT_VALUE_NUMBER(2.71828182845904523536));

    lt_table_set(vm, vm->global, lt_make_string(vm, "math"), t);
}
