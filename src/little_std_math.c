#include "little_std.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

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

static double _lt_math_pop_number(lt_VM* vm, const char* message)
{
    lt_Value val = lt_pop(vm);
    if (!LT_IS_NUMBER(val)) lt_runtime_error(vm, message);
    return LT_GET_NUMBER(val);
}

static uint8_t _lt_clamp(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt_runtime_error(vm, "Expected three arguments to math.clamp!");
    double max = _lt_math_pop_number(vm, "Expected max argument to math.clamp to be number!");
    double min = _lt_math_pop_number(vm, "Expected min argument to math.clamp to be number!");
    double value = _lt_math_pop_number(vm, "Expected value argument to math.clamp to be number!");
    if (value < min) value = min;
    if (value > max) value = max;
    lt_push(vm, LT_VALUE_NUMBER(value));
    return 1;
}

static uint8_t _lt_lerp(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt_runtime_error(vm, "Expected three arguments to math.lerp!");
    double t = _lt_math_pop_number(vm, "Expected t argument to math.lerp to be number!");
    double b = _lt_math_pop_number(vm, "Expected b argument to math.lerp to be number!");
    double a = _lt_math_pop_number(vm, "Expected a argument to math.lerp to be number!");
    lt_push(vm, LT_VALUE_NUMBER(a + (b - a) * t));
    return 1;
}

static uint8_t _lt_sign(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.sign!");
    double value = _lt_math_pop_number(vm, "Expected argument to math.sign to be number!");
    lt_push(vm, LT_VALUE_NUMBER((value > 0.0) - (value < 0.0)));
    return 1;
}

static uint8_t _lt_isnan(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.isnan!");
    double value = _lt_math_pop_number(vm, "Expected argument to math.isnan to be number!");
    lt_push(vm, isnan(value) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_isfinite(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.isfinite!");
    double value = _lt_math_pop_number(vm, "Expected argument to math.isfinite to be number!");
    lt_push(vm, isfinite(value) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_deg(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.deg!");
    double value = _lt_math_pop_number(vm, "Expected argument to math.deg to be number!");
    lt_push(vm, LT_VALUE_NUMBER(value * 180.0 / 3.14159265358979323846));
    return 1;
}

static uint8_t _lt_rad(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.rad!");
    double value = _lt_math_pop_number(vm, "Expected argument to math.rad to be number!");
    lt_push(vm, LT_VALUE_NUMBER(value * 3.14159265358979323846 / 180.0));
    return 1;
}

static uint8_t _lt_random_seeded = 0;

static void _lt_seed_random_once(void)
{
    if (!_lt_random_seeded)
    {
        srand((unsigned int)time(0));
        _lt_random_seeded = 1;
    }
}

static uint8_t _lt_random(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to math.random!");
    _lt_seed_random_once();
    lt_push(vm, LT_VALUE_NUMBER((double)rand() / ((double)RAND_MAX + 1.0)));
    return 1;
}

static uint8_t _lt_randomint(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to math.randomInt!");
    double max_arg = _lt_math_pop_number(vm, "Expected max argument to math.randomInt to be number!");
    double min_arg = _lt_math_pop_number(vm, "Expected min argument to math.randomInt to be number!");
    int32_t min = (int32_t)min_arg;
    int32_t max = (int32_t)max_arg;
    if (max < min) lt_runtime_error(vm, "Expected max to be >= min in math.randomInt!");
    _lt_seed_random_once();
    int32_t value = min + (rand() % (max - min + 1));
    lt_push(vm, LT_VALUE_NUMBER(value));
    return 1;
}

static uint8_t _lt_seed(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.seed!");
    double seed = _lt_math_pop_number(vm, "Expected argument to math.seed to be number!");
    srand((unsigned int)seed);
    _lt_random_seeded = 1;
    return 0;
}

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
    lt_table_set(vm, t, lt_make_string(vm, "clamp"), lt_make_native(vm, _lt_clamp));
    lt_table_set(vm, t, lt_make_string(vm, "lerp"), lt_make_native(vm, _lt_lerp));
    lt_table_set(vm, t, lt_make_string(vm, "sign"), lt_make_native(vm, _lt_sign));
    lt_table_set(vm, t, lt_make_string(vm, "isnan"), lt_make_native(vm, _lt_isnan));
    lt_table_set(vm, t, lt_make_string(vm, "isfinite"), lt_make_native(vm, _lt_isfinite));
    lt_table_set(vm, t, lt_make_string(vm, "deg"), lt_make_native(vm, _lt_deg));
    lt_table_set(vm, t, lt_make_string(vm, "rad"), lt_make_native(vm, _lt_rad));
    lt_table_set(vm, t, lt_make_string(vm, "random"), lt_make_native(vm, _lt_random));
    lt_table_set(vm, t, lt_make_string(vm, "randomInt"), lt_make_native(vm, _lt_randomint));
    lt_table_set(vm, t, lt_make_string(vm, "seed"), lt_make_native(vm, _lt_seed));

    lt_table_set(vm, t, lt_make_string(vm, "pi"), LT_VALUE_NUMBER(3.14159265358979323846));
    lt_table_set(vm, t, lt_make_string(vm, "e"), LT_VALUE_NUMBER(2.71828182845904523536));

    lt_table_set(vm, vm->global, lt_make_string(vm, "math"), t);
}
