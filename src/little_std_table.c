#include "little_internal.h"
#include "little_std.h"

static lt_Value _lt_table_pop_arg(lt_VM* vm, const char* message)
{
    lt_Value table = lt_pop(vm);
    if (!LT_IS_TABLE(table)) lt_runtime_error(vm, message);
    return table;
}

static lt_TablePair* _lt_table_pair_at(lt_Buffer* bucket, uint32_t idx)
{
    return (lt_TablePair*)((char*)bucket->data + (idx * bucket->element_size));
}

static uint8_t _lt_table_has(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to table.has!");
    lt_Value key = lt_pop(vm);
    lt_Value table = _lt_table_pop_arg(vm, "Expected first argument to table.has to be table!");
    lt_push(vm, LT_IS_NULL(lt_table_get(vm, table, key)) ? LT_VALUE_FALSE : LT_VALUE_TRUE);
    return 1;
}

static uint8_t _lt_table_fetch(lt_VM* vm, uint8_t argc)
{
    if (argc < 2 || argc > 3) lt_runtime_error(vm, "Expected 2-3 arguments to table.fetch!");
    lt_Value fallback = LT_VALUE_NULL;
    if (argc == 3) fallback = lt_pop(vm);
    lt_Value key = lt_pop(vm);
    lt_Value table = _lt_table_pop_arg(vm, "Expected first argument to table.fetch to be table!");
    lt_Value value = lt_table_get(vm, table, key);
    lt_push(vm, LT_IS_NULL(value) ? fallback : value);
    return 1;
}

static uint8_t _lt_table_put(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt_runtime_error(vm, "Expected three arguments to table.put!");
    lt_Value value = lt_pop(vm);
    lt_Value key = lt_pop(vm);
    lt_Value table = _lt_table_pop_arg(vm, "Expected first argument to table.put to be table!");
    lt_table_set(vm, table, key, value);
    return 0;
}

static uint8_t _lt_table_remove(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to table.remove!");
    lt_Value key = lt_pop(vm);
    lt_Value table = _lt_table_pop_arg(vm, "Expected first argument to table.remove to be table!");
    lt_table_pop(vm, table, key);
    return 0;
}

static uint8_t _lt_table_keys(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to table.keys!");
    lt_Value table = _lt_table_pop_arg(vm, "Expected argument to table.keys to be table!");
    if (LT_GET_OBJECT(table)->type == LT_OBJECT_SHARED_TABLE)
    {
        lt_push(vm, ltshared_table_keys(vm, LT_GET_OBJECT(table)->shared));
        return 1;
    }
    lt_Value keys = lt_make_array(vm);
    lt_Table* raw = &LT_GET_OBJECT(table)->table;
    for (uint8_t b = 0; b < 16; ++b)
    {
        lt_Buffer* bucket = raw->buckets + b;
        for (uint32_t i = 0; i < bucket->length; ++i)
        {
            lt_TablePair* pair = _lt_table_pair_at(bucket, i);
            if (!LT_IS_NULL(pair->value)) lt_array_push(vm, keys, pair->key);
        }
    }
    lt_push(vm, keys);
    return 1;
}

static uint8_t _lt_table_values(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to table.values!");
    lt_Value table = _lt_table_pop_arg(vm, "Expected argument to table.values to be table!");
    if (LT_GET_OBJECT(table)->type == LT_OBJECT_SHARED_TABLE)
    {
        lt_push(vm, ltshared_table_values(vm, LT_GET_OBJECT(table)->shared));
        return 1;
    }
    lt_Value values = lt_make_array(vm);
    lt_Table* raw = &LT_GET_OBJECT(table)->table;
    for (uint8_t b = 0; b < 16; ++b)
    {
        lt_Buffer* bucket = raw->buckets + b;
        for (uint32_t i = 0; i < bucket->length; ++i)
        {
            lt_TablePair* pair = _lt_table_pair_at(bucket, i);
            if (!LT_IS_NULL(pair->value)) lt_array_push(vm, values, pair->value);
        }
    }
    lt_push(vm, values);
    return 1;
}

void ltstd_open_table(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    lt_table_set(vm, t, lt_make_string(vm, "has"), lt_make_native(vm, _lt_table_has));
    lt_table_set(vm, t, lt_make_string(vm, "fetch"), lt_make_native(vm, _lt_table_fetch));
    lt_table_set(vm, t, lt_make_string(vm, "put"), lt_make_native(vm, _lt_table_put));
    lt_table_set(vm, t, lt_make_string(vm, "remove"), lt_make_native(vm, _lt_table_remove));
    lt_table_set(vm, t, lt_make_string(vm, "keys"), lt_make_native(vm, _lt_table_keys));
    lt_table_set(vm, t, lt_make_string(vm, "values"), lt_make_native(vm, _lt_table_values));

    lt_table_set(vm, vm->global, lt_make_string(vm, "table"), t);
}
