#include "little_std.h"
#include "pcg_basic.h"

#include <openssl/rand.h>
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
    lt_push(vm, LT_VALUE_NUMBER(name(LT_GET_NUMBER(arg2), LT_GET_NUMBER(arg1)))); \
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

static void _lt_seed_random_once(lt_VM* vm)
{
    if (vm->rng_seeded) return;
    uint64_t seed = 0;
    uint64_t sequence = 0;
    if (RAND_bytes((unsigned char*)&seed, (int)sizeof(seed)) != 1)
        seed = (uint64_t)time(0) ^ (uint64_t)(uintptr_t)vm;
    if (RAND_bytes((unsigned char*)&sequence, (int)sizeof(sequence)) != 1)
        sequence = 0xda3e39cb94b95bdbULL;
    pcg32_random_t rng;
    pcg32_srandom_r(&rng, seed, sequence);
    vm->rng_state = rng.state;
    vm->rng_inc = rng.inc;
    vm->rng_seeded = 1;
}

static uint32_t _lt_random_next(lt_VM* vm)
{
    _lt_seed_random_once(vm);
    pcg32_random_t rng = { vm->rng_state, vm->rng_inc };
    uint32_t value = pcg32_random_r(&rng);
    vm->rng_state = rng.state;
    vm->rng_inc = rng.inc;
    return value;
}

static uint8_t _lt_random(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to math.random!");
    uint64_t value = ((uint64_t)_lt_random_next(vm) << 32) | _lt_random_next(vm);
    lt_push(vm, LT_VALUE_NUMBER((double)(value >> 11) * (1.0 / 9007199254740992.0)));
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
    uint64_t range = (uint64_t)((int64_t)max - (int64_t)min) + 1;
    uint32_t offset;
    if (range > UINT32_MAX) offset = _lt_random_next(vm);
    else
    {
        pcg32_random_t rng = { vm->rng_state, vm->rng_inc };
        offset = pcg32_boundedrand_r(&rng, (uint32_t)range);
        vm->rng_state = rng.state;
        vm->rng_inc = rng.inc;
    }
    int32_t value = min + (int32_t)offset;
    lt_push(vm, LT_VALUE_NUMBER(value));
    return 1;
}

static uint8_t _lt_seed(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to math.seed!");
    double seed = _lt_math_pop_number(vm, "Expected argument to math.seed to be number!");
    pcg32_random_t rng;
    pcg32_srandom_r(&rng, (uint64_t)(uint32_t)seed, 0xda3e39cb94b95bdbULL);
    vm->rng_state = rng.state;
    vm->rng_inc = rng.inc;
    vm->rng_seeded = 1;
    return 0;
}

void ltstd_open_math(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    /** [LittleBinding] math.sin
     *  returns the sine of an angle in radians.
     *  @signature (x)
     *  @param x angle in radians
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "sin"), lt_make_native(vm, _lt_sin));
    /** [LittleBinding] math.cos
     *  returns the cosine of an angle in radians.
     *  @signature (x)
     *  @param x angle in radians
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "cos"), lt_make_native(vm, _lt_cos));
    /** [LittleBinding] math.tan
     *  returns the tangent of an angle in radians.
     *  @signature (x)
     *  @param x angle in radians
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "tan"), lt_make_native(vm, _lt_tan));

    /** [LittleBinding] math.asin
     *  returns the angle in radians whose sine is the given value.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "asin"), lt_make_native(vm, _lt_asin));
    /** [LittleBinding] math.acos
     *  returns the angle in radians whose cosine is the given value.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "acos"), lt_make_native(vm, _lt_acos));
    /** [LittleBinding] math.atan
     *  returns the angle in radians whose tangent is the given value.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "atan"), lt_make_native(vm, _lt_atan));

    /** [LittleBinding] math.sinh
     *  returns the hyperbolic sine of a number.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "sinh"), lt_make_native(vm, _lt_sinh));
    /** [LittleBinding] math.cosh
     *  returns the hyperbolic cosine of a number.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "cosh"), lt_make_native(vm, _lt_cosh));
    /** [LittleBinding] math.tanh
     *  returns the hyperbolic tangent of a number.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "tanh"), lt_make_native(vm, _lt_tanh));

    /** [LittleBinding] math.floor
     *  returns the largest whole number that is not greater than the value.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "floor"), lt_make_native(vm, _lt_floor));
    /** [LittleBinding] math.ceil
     *  returns the smallest whole number that is not less than the value.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "ceil"),  lt_make_native(vm, _lt_ceil));
    /** [LittleBinding] math.round
     *  returns the nearest whole number, rounding halves away from zero.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "round"), lt_make_native(vm, _lt_round));

    /** [LittleBinding] math.exp
     *  returns e raised to the given power.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "exp"),   lt_make_native(vm, _lt_exp));
    /** [LittleBinding] math.log
     *  returns the natural logarithm of a number.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "log"),   lt_make_native(vm, _lt_log));
    /** [LittleBinding] math.log10
     *  returns the base-10 logarithm of a number.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "log10"), lt_make_native(vm, _lt_log10));
    /** [LittleBinding] math.sqrt
     *  returns the square root of a number, which is NaN for negative values.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "sqrt"),  lt_make_native(vm, _lt_sqrt));
    /** [LittleBinding] math.abs
     *  returns the absolute value of a number.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "abs"),   lt_make_native(vm, _lt_fabs));

    /** [LittleBinding] math.min
     *  returns the smaller of two numbers.
     *  @signature (a, b)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "min"), lt_make_native(vm, _lt_fmin));
    /** [LittleBinding] math.max
     *  returns the larger of two numbers.
     *  @signature (a, b)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "max"), lt_make_native(vm, _lt_fmax));
    /** [LittleBinding] math.pow
     *  returns a raised to the power b.
     *  @signature (a, b)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "pow"), lt_make_native(vm, _lt_pow));
    /** [LittleBinding] math.mod
     *  returns the remainder of dividing a by b, taking the sign of a.
     *  @signature (a, b)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "mod"), lt_make_native(vm, _lt_fmod));
    /** [LittleBinding] math.clamp
     *  restricts x to the inclusive range between min and max.
     *  @signature (x, min, max)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "clamp"), lt_make_native(vm, _lt_clamp));
    /** [LittleBinding] math.lerp
     *  linearly interpolates from a to b, where t of 0 gives a and 1 gives b.
     *  @signature (a, b, t)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "lerp"), lt_make_native(vm, _lt_lerp));
    /** [LittleBinding] math.sign
     *  returns -1, 0 or 1 according to the sign of the value.
     *  @signature (x)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "sign"), lt_make_native(vm, _lt_sign));
    /** [LittleBinding] math.isnan
     *  returns true when the value is NaN.
     *  @signature (x)
     *  @return bool
     */
    lt_table_set(vm, t, lt_make_string(vm, "isnan"), lt_make_native(vm, _lt_isnan));
    /** [LittleBinding] math.isfinite
     *  returns true when the value is neither infinite nor NaN.
     *  @signature (x)
     *  @return bool
     */
    lt_table_set(vm, t, lt_make_string(vm, "isfinite"), lt_make_native(vm, _lt_isfinite));
    /** [LittleBinding] math.deg
     *  converts an angle from radians to degrees.
     *  @signature (radians)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "deg"), lt_make_native(vm, _lt_deg));
    /** [LittleBinding] math.rad
     *  converts an angle from degrees to radians.
     *  @signature (degrees)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "rad"), lt_make_native(vm, _lt_rad));
    /** [LittleBinding] math.random
     *  returns a uniformly distributed random number in the range [0, 1).
     *  @signature ()
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "random"), lt_make_native(vm, _lt_random));
    /** [LittleBinding] math.randomInt
     *  returns a uniformly distributed whole number between min and max, inclusive.
     *  @signature (min, max)
     *  @return number
     */
    lt_table_set(vm, t, lt_make_string(vm, "randomInt"), lt_make_native(vm, _lt_randomint));
    /** [LittleBinding] math.seed
     *  seeds the random generator from n so later values are reproducible, returning nothing.
     *  @signature (n)
     *  @return null
     */
    lt_table_set(vm, t, lt_make_string(vm, "seed"), lt_make_native(vm, _lt_seed));

    lt_table_set(vm, t, lt_make_string(vm, "pi"), LT_VALUE_NUMBER(3.14159265358979323846));
    lt_table_set(vm, t, lt_make_string(vm, "e"), LT_VALUE_NUMBER(2.71828182845904523536));

    lt_table_set(vm, vm->global, lt_make_string(vm, "math"), t);
}
