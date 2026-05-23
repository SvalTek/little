#include "little_std.h"

static lt_Value _lt_array_pop_arg(lt_VM* vm, const char* message)
{
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, message);
    return arr;
}

static uint8_t _lt_array_next(lt_VM* vm, uint8_t argc)
{
    lt_Value current = lt_getupval(vm, 1);
    lt_Value arr = lt_getupval(vm, 0);

    uint32_t idx = lt_get_number(current);
    lt_Value to_return = idx >= lt_array_length(arr) ? LT_VALUE_NULL : *lt_array_at(arr, idx);

    lt_setupval(vm, 1, LT_VALUE_NUMBER(idx + 1));
    lt_push(vm, to_return);

    return 1;
}

static uint8_t _lt_array_each(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.each!");

    lt_Value arr = lt_pop(vm);

    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.each to be array!");

    lt_push(vm, lt_make_native(vm, _lt_array_next));
    lt_push(vm, LT_VALUE_NUMBER(0));
    lt_push(vm, arr);
    lt_close(vm, 2);

    return 1;
}

static uint8_t _lt_range_iter(lt_VM* vm, uint8_t argc)
{
    lt_Value start = lt_getupval(vm, 2);
    lt_Value end = lt_getupval(vm, 1);
    lt_Value step = lt_getupval(vm, 0);
    double current = lt_get_number(start);
    double limit = lt_get_number(end);
    double amount = lt_get_number(step);

    if (amount == 0) lt_runtime_error(vm, "Expected array.range step to be non-zero!");
    if ((amount > 0 && current >= limit) || (amount < 0 && current <= limit))
    {
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_setupval(vm, 2, lt_make_number(current + amount));

    lt_push(vm, start);
    return 1;
}

static uint8_t _lt_range(lt_VM* vm, uint8_t argc)
{
    lt_Value start = LT_VALUE_NUMBER(0);
    lt_Value end = LT_VALUE_NUMBER(0);
    lt_Value step = LT_VALUE_NUMBER(1);

    if (argc == 1)
    {
        end = lt_pop(vm);
    }
    else if (argc == 2)
    {
        end = lt_pop(vm);
        start = lt_pop(vm);
    }
    else if (argc == 3)
    {
        step = lt_pop(vm);
        end = lt_pop(vm);
        start = lt_pop(vm);
    }
    else lt_runtime_error(vm, "Expected 1-3 args for array.range([start,] end [, step]!");

    if (!LT_IS_NUMBER(start) || !LT_IS_NUMBER(end) || !LT_IS_NUMBER(step))
        lt_runtime_error(vm, "Expected all arguments to array.range to be numbers!");
    if (LT_GET_NUMBER(step) == 0) lt_runtime_error(vm, "Expected array.range step to be non-zero!");

    lt_push(vm, lt_make_native(vm, _lt_range_iter));
    lt_push(vm, start);
    lt_push(vm, end);
    lt_push(vm, step);
    lt_close(vm, 3);

    return 1;
}

static uint8_t _lt_array_len(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.len!");
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.len to be array!");

    lt_push(vm, lt_make_number(lt_array_length(arr)));
    return 1;
}

static uint8_t _lt_array_pop(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.pop!");
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.pop to be array!");
    if (lt_array_length(arr) == 0) lt_runtime_error(vm, "Cannot pop from empty array!");

    lt_push(vm, lt_array_remove(vm, arr, lt_array_length(arr) - 1));
    return 1;
}

static uint8_t _lt_array_last(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.last!");
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.last to be array!");
    if (lt_array_length(arr) == 0) lt_runtime_error(vm, "Expected argument to array.last to be non-empty!");

    lt_push(vm, *lt_array_at(arr, lt_array_length(arr) - 1));
    return 1;
}

static uint8_t _lt_array_first(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.first!");
    lt_Value arr = _lt_array_pop_arg(vm, "Expected argument to array.first to be array!");
    if (lt_array_length(arr) == 0) lt_runtime_error(vm, "Expected argument to array.first to be non-empty!");

    lt_push(vm, *lt_array_at(arr, 0));
    return 1;
}

static uint8_t _lt_array_push(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to array.push!");
    lt_Value val = lt_pop(vm);
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected first argument to array.push to be array!");

    lt_array_push(vm, arr, val);
    return 0;
}

static uint8_t _lt_array_clear(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.clear!");
    lt_Value arr = _lt_array_pop_arg(vm, "Expected argument to array.clear to be array!");
    LT_GET_OBJECT(arr)->array.length = 0;
    return 0;
}

static uint8_t _lt_array_insert(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt_runtime_error(vm, "Expected three arguments to array.insert!");
    lt_Value val = lt_pop(vm);
    lt_Value idxval = lt_pop(vm);
    lt_Value arr = _lt_array_pop_arg(vm, "Expected first argument to array.insert to be array!");
    if (!LT_IS_NUMBER(idxval)) lt_runtime_error(vm, "Expected second argument to array.insert to be number!");

    uint32_t len = lt_array_length(arr);
    uint32_t idx = (uint32_t)LT_GET_NUMBER(idxval);
    if (idx > len) idx = len;

    lt_array_push(vm, arr, val);
    for (uint32_t i = len; i > idx; --i)
    {
        *lt_array_at(arr, i) = *lt_array_at(arr, i - 1);
    }
    *lt_array_at(arr, idx) = val;
    return 0;
}

static uint8_t _lt_array_remove(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to array.remove!");
    lt_Value idx = lt_pop(vm);
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected first argument to array.remove to be array!");
    if (!LT_IS_NUMBER(idx)) lt_runtime_error(vm, "Expected second argument to array.remove to be number!");

    lt_array_remove(vm, arr, (uint32_t)lt_get_number(idx));
    return 0;
}

static uint8_t _lt_array_contains(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to array.contains!");
    lt_Value val = lt_pop(vm);
    lt_Value arr = _lt_array_pop_arg(vm, "Expected first argument to array.contains to be array!");
    for (uint32_t i = 0; i < lt_array_length(arr); ++i)
    {
        if (lt_equals(*lt_array_at(arr, i), val))
        {
            lt_push(vm, LT_VALUE_TRUE);
            return 1;
        }
    }
    lt_push(vm, LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_array_indexof(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to array.indexOf!");
    lt_Value val = lt_pop(vm);
    lt_Value arr = _lt_array_pop_arg(vm, "Expected first argument to array.indexOf to be array!");
    for (uint32_t i = 0; i < lt_array_length(arr); ++i)
    {
        if (lt_equals(*lt_array_at(arr, i), val))
        {
            lt_push(vm, LT_VALUE_NUMBER(i));
            return 1;
        }
    }
    lt_push(vm, LT_VALUE_NUMBER(-1));
    return 1;
}

static uint8_t _lt_array_join(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) lt_runtime_error(vm, "Expected 1-2 arguments to array.join!");
    const char* sep = "";
    if (argc == 2)
    {
        lt_Value sepval = lt_pop(vm);
        if (!LT_IS_STRING(sepval)) lt_runtime_error(vm, "Expected separator argument to array.join to be string!");
        sep = lt_get_string(vm, sepval);
    }
    lt_Value arr = _lt_array_pop_arg(vm, "Expected first argument to array.join to be array!");

    char* out = 0;
    uint32_t len = 0;
    for (uint32_t i = 0; i < lt_array_length(arr); ++i)
    {
        char* part = ltstd_tostring(vm, *lt_array_at(arr, i));
        uint32_t part_len = 0;
        while (part[part_len]) ++part_len;
        uint32_t sep_len = 0;
        while (sep[sep_len]) ++sep_len;
        uint32_t add_sep = i > 0 ? sep_len : 0;
        char* next = vm->alloc(len + add_sep + part_len + 1);
        if (out)
        {
            for (uint32_t j = 0; j < len; ++j) next[j] = out[j];
            vm->free(out);
        }
        if (add_sep) for (uint32_t j = 0; j < sep_len; ++j) next[len + j] = sep[j];
        for (uint32_t j = 0; j < part_len; ++j) next[len + add_sep + j] = part[j];
        len += add_sep + part_len;
        next[len] = 0;
        out = next;
        vm->free(part);
    }

    lt_push(vm, lt_make_string(vm, out ? out : ""));
    if (out) vm->free(out);
    return 1;
}

static uint8_t _lt_array_reverse(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.reverse!");
    lt_Value arr = _lt_array_pop_arg(vm, "Expected argument to array.reverse to be array!");
    lt_Value reversed = lt_make_array(vm);
    uint32_t len = lt_array_length(arr);
    for (uint32_t i = 0; i < len; ++i)
    {
        lt_array_push(vm, reversed, *lt_array_at(arr, len - 1 - i));
    }
    lt_push(vm, reversed);
    return 1;
}

static uint8_t _lt_array_slice(lt_VM* vm, uint8_t argc)
{
    if (argc < 2 || argc > 3) lt_runtime_error(vm, "Expected 2-3 arguments to array.slice!");
    lt_Value countval = LT_VALUE_NULL;
    if (argc == 3) countval = lt_pop(vm);
    lt_Value startval = lt_pop(vm);
    lt_Value arr = _lt_array_pop_arg(vm, "Expected first argument to array.slice to be array!");
    if (!LT_IS_NUMBER(startval)) lt_runtime_error(vm, "Expected start argument to array.slice to be number!");
    uint32_t len = lt_array_length(arr);
    uint32_t start = (uint32_t)LT_GET_NUMBER(startval);
    if (start > len) start = len;
    uint32_t count = len - start;
    if (argc == 3)
    {
        if (!LT_IS_NUMBER(countval)) lt_runtime_error(vm, "Expected count argument to array.slice to be number!");
        count = (uint32_t)LT_GET_NUMBER(countval);
        if (start + count > len) count = len - start;
    }

    lt_Value sliced = lt_make_array(vm);
    for (uint32_t i = 0; i < count; ++i)
    {
        lt_array_push(vm, sliced, *lt_array_at(arr, start + i));
    }
    lt_push(vm, sliced);
    return 1;
}

void ltstd_open_array(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    lt_table_set(vm, t, lt_make_string(vm, "each"), lt_make_native(vm, _lt_array_each));
    lt_table_set(vm, t, lt_make_string(vm, "range"), lt_make_native(vm, _lt_range));

    lt_table_set(vm, t, lt_make_string(vm, "len"), lt_make_native(vm, _lt_array_len));
    lt_table_set(vm, t, lt_make_string(vm, "first"), lt_make_native(vm, _lt_array_first));
    lt_table_set(vm, t, lt_make_string(vm, "last"), lt_make_native(vm, _lt_array_last));
    lt_table_set(vm, t, lt_make_string(vm, "pop"), lt_make_native(vm, _lt_array_pop));
    lt_table_set(vm, t, lt_make_string(vm, "push"), lt_make_native(vm, _lt_array_push));
    lt_table_set(vm, t, lt_make_string(vm, "clear"), lt_make_native(vm, _lt_array_clear));
    lt_table_set(vm, t, lt_make_string(vm, "insert"), lt_make_native(vm, _lt_array_insert));
    lt_table_set(vm, t, lt_make_string(vm, "remove"), lt_make_native(vm, _lt_array_remove));
    lt_table_set(vm, t, lt_make_string(vm, "contains"), lt_make_native(vm, _lt_array_contains));
    lt_table_set(vm, t, lt_make_string(vm, "indexOf"), lt_make_native(vm, _lt_array_indexof));
    lt_table_set(vm, t, lt_make_string(vm, "join"), lt_make_native(vm, _lt_array_join));
    lt_table_set(vm, t, lt_make_string(vm, "reverse"), lt_make_native(vm, _lt_array_reverse));
    lt_table_set(vm, t, lt_make_string(vm, "slice"), lt_make_native(vm, _lt_array_slice));

    lt_table_set(vm, vm->global, lt_make_string(vm, "array"), t);
}
