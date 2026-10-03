#include "little_std.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* number is about the number type itself: coercion, formatting, predicates,
   ranges, and limits. Arithmetic lives in math and bitwise work in bit, so the
   three modules do not overlap. */

#define LT_NUMBER_LIMIT 9007199254740991.0   /* 2^53 - 1, the largest exact integer */

static double _lt_number_expect(lt_VM* vm, lt_Value value, const char* message)
{
    if (!LT_IS_NUMBER(value)) lt_runtime_error(vm, message);
    return LT_GET_NUMBER(value);
}

static double _lt_number_pop(lt_VM* vm, const char* name)
{
    char message[96];
    snprintf(message, sizeof(message), "Expected argument to number.%s to be number!", name);
    return _lt_number_expect(vm, lt_pop(vm), message);
}

/* Discrete parameters (bases, decimal places) must be whole numbers: a
   fractional value would be silently truncated by the cast, changing its
   meaning, and roundTo would raise ten to a fractional power. The comparison
   form also rejects NaN, which fails every comparison. */
static double _lt_number_whole(lt_VM* vm, lt_Value value, const char* what, double minimum, double maximum)
{
    char message[128];
    snprintf(message, sizeof(message), "Expected argument to number.%s to be number!", what);
    double number = _lt_number_expect(vm, value, message);
    if (!(number >= minimum && number <= maximum) || number != floor(number))
    {
        snprintf(message, sizeof(message), "Expected number.%s between %g and %g, as a whole number!", what, minimum, maximum);
        lt_runtime_error(vm, message);
    }
    return number;
}

/* Shared by number.from: skips surrounding whitespace and reports where the
   meaningful text starts. Returns 0 when the text is blank. */
static const char* _lt_number_trim(const char* text, const char** end)
{
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    const char* tail = text + strlen(text);
    while (tail > text && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\n' || tail[-1] == '\r')) tail--;
    *end = tail;
    return text;
}

static uint8_t _lt_number_parse_radix(const char* digits, const char* limit, int radix, double sign, double* out)
{
    double value = 0;
    uint8_t any = 0;
    for (const char* p = digits; p < limit; ++p)
    {
        int digit = -1;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'z') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') digit = *p - 'A' + 10;
        if (digit < 0 || digit >= radix) return 0;
        value = value * radix + digit;
        any = 1;
    }
    if (!any) return 0;
    *out = sign * value;
    return 1;
}

/* Parses the same forms the tokenizer accepts: decimal with an optional
   fraction and exponent, 0x hex, and 0b binary. Anything else is null rather
   than an error, so a caller can test a string without trapping. */
static uint8_t _lt_number_from(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to number.from!");
    lt_Value value = lt_pop(vm);

    if (LT_IS_NUMBER(value))
    {
        lt_push(vm, value);
        return 1;
    }
    if (!LT_IS_STRING(value))
    {
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    const char* limit = 0;
    const char* text = _lt_number_trim(lt_get_string(vm, value), &limit);
    if (text >= limit)
    {
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    double sign = 1;
    if (*text == '+' || *text == '-')
    {
        if (*text == '-') sign = -1;
        text++;
        if (text >= limit)
        {
            lt_push(vm, LT_VALUE_NULL);
            return 1;
        }
    }

    double parsed = 0;
    if (limit - text > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        if (!_lt_number_parse_radix(text + 2, limit, 16, sign, &parsed))
        {
            lt_push(vm, LT_VALUE_NULL);
            return 1;
        }
    }
    else if (limit - text > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
    {
        if (!_lt_number_parse_radix(text + 2, limit, 2, sign, &parsed))
        {
            lt_push(vm, LT_VALUE_NULL);
            return 1;
        }
    }
    else
    {
        /* strtod handles the fraction and exponent, but it accepts prefixes
           such as "inf" and "nan" too. Infinity round-trips through
           string.from, so it is accepted; NaN is not, because the boxed value
           representation tags non-numbers with the quiet-NaN pattern, so a
           stored NaN is indistinguishable from a tag - and strtod also accepts
           implementation-defined payloads such as "nan(0x5000000000001)",
           whose bits can look like a tag and be dereferenced. The sign was
           stripped above, so it is reapplied here rather than passed through. */
        char* end = 0;
        parsed = sign * strtod(text, &end);
        if (end != limit || isnan(parsed))
        {
            lt_push(vm, LT_VALUE_NULL);
            return 1;
        }
    }

    lt_push(vm, LT_VALUE_NUMBER(parsed));
    return 1;
}

/* Formats with a fixed number of decimals, optionally grouping thousands.
   string.from stays the plain coercion; this is the precision-controlled one. */
static uint8_t _lt_number_format(lt_VM* vm, uint8_t argc)
{
    if (argc != 2 && argc != 3) lt_runtime_error(vm, "Expected a number and decimals, and optional separators, to number.format!");
    lt_Value separators = argc == 3 ? lt_pop(vm) : LT_VALUE_FALSE;
    lt_Value decimals_value = lt_pop(vm);
    lt_Value value = lt_pop(vm);
    char message[96];
    snprintf(message, sizeof(message), "Expected argument to number.format to be number!");
    double number = _lt_number_expect(vm, value, message);
    double decimals = _lt_number_whole(vm, decimals_value, "format decimals", 0, 32);
    if (!LT_IS_BOOL(separators)) lt_runtime_error(vm, "Expected number.format separators to be boolean!");

    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%.*f", (int)decimals, number);

    if (separators == LT_VALUE_TRUE)
    {
        char grouped[768];
        size_t out = 0;
        const char* p = buffer;
        if (*p == '-')
        {
            grouped[out++] = *p++;
        }
        const char* dot = strchr(p, '.');
        size_t whole = dot ? (size_t)(dot - p) : strlen(p);
        for (size_t i = 0; i < whole; ++i)
        {
            if (i > 0 && (whole - i) % 3 == 0) grouped[out++] = ',';
            grouped[out++] = p[i];
        }
        if (dot)
        {
            size_t rest = strlen(dot);
            if (out + rest + 1 >= sizeof(grouped)) lt_runtime_error(vm, "number.format result is too long!");
            memcpy(grouped + out, dot, rest);
            out += rest;
        }
        grouped[out] = 0;
        lt_push(vm, lt_make_string(vm, grouped));
        return 1;
    }

    lt_push(vm, lt_make_string(vm, buffer));
    return 1;
}

/* Renders in a given base with an optional prefix, so number.from can read the
   result back: toBase(255, 16) is "ff", toBase(255, 16, true) is "0xff". */
static uint8_t _lt_number_tobase(lt_VM* vm, uint8_t argc)
{
    if (argc != 2 && argc != 3) lt_runtime_error(vm, "Expected a number and a base, and optional prefix, to number.toBase!");
    lt_Value prefix = argc == 3 ? lt_pop(vm) : LT_VALUE_FALSE;
    lt_Value base_value = lt_pop(vm);
    lt_Value value = lt_pop(vm);
    double number = _lt_number_expect(vm, value, "Expected argument to number.toBase to be number!");
    double base = _lt_number_whole(vm, base_value, "toBase base", 2, 36);
    if (!LT_IS_BOOL(prefix)) lt_runtime_error(vm, "Expected number.toBase prefix to be boolean!");
    if (!isfinite(number) || number != floor(number)) lt_runtime_error(vm, "Expected number.toBase value to be a whole number!");

    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    int radix = (int)base;
    uint8_t negative = number < 0;
    double remaining = negative ? -number : number;
    char reversed[80];
    int length = 0;
    do
    {
        if (length >= (int)sizeof(reversed)) lt_runtime_error(vm, "number.toBase value is too large!");
        reversed[length++] = digits[(int)fmod(remaining, (double)radix)];
        remaining = floor(remaining / radix);
    } while (remaining > 0);

    char text[96];
    int out = 0;
    if (negative) text[out++] = '-';
    if (prefix == LT_VALUE_TRUE)
    {
        if (radix == 16) { text[out++] = '0'; text[out++] = 'x'; }
        else if (radix == 2) { text[out++] = '0'; text[out++] = 'b'; }
    }
    for (int i = length - 1; i >= 0; --i) text[out++] = reversed[i];
    text[out] = 0;
    lt_push(vm, lt_make_string(vm, text));
    return 1;
}

static uint8_t _lt_number_isinteger(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to number.isInteger!");
    double value = _lt_number_pop(vm, "isInteger");
    lt_push(vm, isfinite(value) && value == floor(value) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_number_issafeinteger(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to number.isSafeInteger!");
    double value = _lt_number_pop(vm, "isSafeInteger");
    uint8_t safe = isfinite(value) && value == floor(value) && fabs(value) <= LT_NUMBER_LIMIT;
    lt_push(vm, safe ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

/* Compares with a tolerance, which is what float equality actually needs. */
static uint8_t _lt_number_isclose(lt_VM* vm, uint8_t argc)
{
    if (argc != 2 && argc != 3) lt_runtime_error(vm, "Expected two numbers, and an optional epsilon, to number.isClose!");
    lt_Value epsilon_value = argc == 3 ? lt_pop(vm) : LT_VALUE_NULL;
    double b = _lt_number_pop(vm, "isClose");
    double a = _lt_number_pop(vm, "isClose");
    double epsilon = 1e-9;
    if (!LT_IS_NULL(epsilon_value))
    {
        epsilon = _lt_number_expect(vm, epsilon_value, "Expected number.isClose epsilon to be number!");
        if (!(epsilon >= 0)) lt_runtime_error(vm, "Expected number.isClose epsilon to be positive!");
    }
    if (a == b)
    {
        lt_push(vm, LT_VALUE_TRUE);
        return 1;
    }
    /* An infinite operand makes both the difference and the scaled tolerance
       infinite, and INFINITY <= INFINITY would report unrelated values as
       close, so anything non-finite is simply not close. */
    if (!isfinite(a) || !isfinite(b))
    {
        lt_push(vm, LT_VALUE_FALSE);
        return 1;
    }
    double difference = fabs(a - b);
    double scale = fmax(fabs(a), fabs(b));
    double tolerance = epsilon * (scale > 1 ? scale : 1);
    lt_push(vm, difference <= tolerance ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

/* Truncates towards zero, which floor and ceil cannot express for negatives. */
static uint8_t _lt_number_trunc(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to number.trunc!");
    double value = _lt_number_pop(vm, "trunc");
    lt_push(vm, LT_VALUE_NUMBER(value < 0 ? ceil(value) : floor(value)));
    return 1;
}

static uint8_t _lt_number_roundto(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a number and decimals to number.roundTo!");
    double decimals = _lt_number_whole(vm, lt_pop(vm), "roundTo decimals", 0, 15);
    double value = _lt_number_pop(vm, "roundTo");
    double scale = pow(10.0, decimals);
    /* Scaling first can overflow for very large values, which have no
       fractional part at this precision anyway, so they are returned as-is
       rather than turned into infinity. */
    if (!isfinite(value * scale))
    {
        lt_push(vm, LT_VALUE_NUMBER(value));
        return 1;
    }
    lt_push(vm, LT_VALUE_NUMBER(round(value * scale) / scale));
    return 1;
}

static uint8_t _lt_number_snap(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a number and a step to number.snap!");
    double step = _lt_number_pop(vm, "snap");
    double value = _lt_number_pop(vm, "snap");
    if (step == 0) lt_runtime_error(vm, "Expected number.snap step to be non-zero!");
    lt_push(vm, LT_VALUE_NUMBER(round(value / step) * step));
    return 1;
}

static uint8_t _lt_number_wrap(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt_runtime_error(vm, "Expected a number, a minimum, and a maximum to number.wrap!");
    double maximum = _lt_number_pop(vm, "wrap");
    double minimum = _lt_number_pop(vm, "wrap");
    double value = _lt_number_pop(vm, "wrap");
    if (!isfinite(value) || !isfinite(minimum) || !isfinite(maximum))
        lt_runtime_error(vm, "Expected number.wrap arguments to be finite!");
    if (!(maximum > minimum)) lt_runtime_error(vm, "Expected number.wrap maximum to be greater than minimum!");
    double span = maximum - minimum;
    double wrapped = fmod(value - minimum, span);
    if (wrapped < 0) wrapped += span;
    lt_push(vm, LT_VALUE_NUMBER(minimum + wrapped));
    return 1;
}

static uint8_t _lt_number_pingpong(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected a number and a length to number.pingPong!");
    double length = _lt_number_pop(vm, "pingPong");
    double value = _lt_number_pop(vm, "pingPong");
    if (!isfinite(value) || !isfinite(length)) lt_runtime_error(vm, "Expected number.pingPong arguments to be finite!");
    if (!(length > 0)) lt_runtime_error(vm, "Expected number.pingPong length to be positive!");
    double span = length * 2;
    double wrapped = fmod(value, span);
    if (wrapped < 0) wrapped += span;
    lt_push(vm, LT_VALUE_NUMBER(wrapped > length ? span - wrapped : wrapped));
    return 1;
}

static uint8_t _lt_number_map(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt_runtime_error(vm, "Expected a number and four range bounds to number.map!");
    double out_max = _lt_number_pop(vm, "map");
    double out_min = _lt_number_pop(vm, "map");
    double in_max = _lt_number_pop(vm, "map");
    double in_min = _lt_number_pop(vm, "map");
    double value = _lt_number_pop(vm, "map");
    if (in_max == in_min) lt_runtime_error(vm, "Expected number.map input range to be non-empty!");
    lt_push(vm, LT_VALUE_NUMBER(out_min + (value - in_min) * (out_max - out_min) / (in_max - in_min)));
    return 1;
}

void ltstd_open_number(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);

    lt_table_set(vm, t, lt_make_string(vm, "from"), lt_make_native(vm, _lt_number_from));
    lt_table_set(vm, t, lt_make_string(vm, "format"), lt_make_native(vm, _lt_number_format));
    lt_table_set(vm, t, lt_make_string(vm, "toBase"), lt_make_native(vm, _lt_number_tobase));

    lt_table_set(vm, t, lt_make_string(vm, "isInteger"), lt_make_native(vm, _lt_number_isinteger));
    lt_table_set(vm, t, lt_make_string(vm, "isSafeInteger"), lt_make_native(vm, _lt_number_issafeinteger));
    lt_table_set(vm, t, lt_make_string(vm, "isClose"), lt_make_native(vm, _lt_number_isclose));

    lt_table_set(vm, t, lt_make_string(vm, "trunc"), lt_make_native(vm, _lt_number_trunc));
    lt_table_set(vm, t, lt_make_string(vm, "roundTo"), lt_make_native(vm, _lt_number_roundto));
    lt_table_set(vm, t, lt_make_string(vm, "snap"), lt_make_native(vm, _lt_number_snap));
    lt_table_set(vm, t, lt_make_string(vm, "wrap"), lt_make_native(vm, _lt_number_wrap));
    lt_table_set(vm, t, lt_make_string(vm, "pingPong"), lt_make_native(vm, _lt_number_pingpong));
    lt_table_set(vm, t, lt_make_string(vm, "map"), lt_make_native(vm, _lt_number_map));

    lt_table_set(vm, t, lt_make_string(vm, "epsilon"), LT_VALUE_NUMBER(DBL_EPSILON));
    lt_table_set(vm, t, lt_make_string(vm, "infinity"), LT_VALUE_NUMBER(INFINITY));
    /* There is deliberately no number.nan: the value representation tags
       non-numbers with the quiet-NaN bit pattern, so a real NaN is
       indistinguishable from a tag and cannot be stored. Infinity is fine,
       because it keeps a zero mantissa. */
    lt_table_set(vm, t, lt_make_string(vm, "maxSafeInteger"), LT_VALUE_NUMBER(LT_NUMBER_LIMIT));
    lt_table_set(vm, t, lt_make_string(vm, "minSafeInteger"), LT_VALUE_NUMBER(-LT_NUMBER_LIMIT));

    lt_table_set(vm, vm->global, lt_make_string(vm, "number"), t);
}
