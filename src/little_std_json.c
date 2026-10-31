#include "little_std.h"
#include "little_internal.h"

#include "yyjson.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LITTLE_JSON_DEPTH_LIMIT 64

static uint8_t json_to_little(lt_VM* vm, yyjson_val* value, lt_Value* out, uint8_t depth);

static uint8_t json_string_to_little(lt_VM* vm, yyjson_val* value, lt_Value* out)
{
    const char* text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    if (!text) return 0;
    if (memchr(text, 0, length) || text[length] != 0) return 0;
    *out = lt_make_string(vm, text);
    return 1;
}

static uint8_t json_array_to_little(lt_VM* vm, yyjson_val* value, lt_Value* out, uint8_t depth)
{
    lt_Value array = lt_make_array(vm);
    lt_push(vm, array);
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(value, &iter);
    yyjson_val* item;
    while ((item = yyjson_arr_iter_next(&iter)))
    {
        lt_Value child = LT_VALUE_NULL;
        if (!json_to_little(vm, item, &child, depth + 1))
        {
            lt_pop(vm);
            return 0;
        }
        lt_push(vm, child);
        lt_array_push(vm, array, child);
        lt_pop(vm);
    }
    lt_pop(vm);
    *out = array;
    return 1;
}

static uint8_t json_object_to_little(lt_VM* vm, yyjson_val* value, lt_Value* out, uint8_t depth)
{
    lt_Value table = lt_make_table(vm);
    lt_push(vm, table);
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(value, &iter);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter)))
    {
        lt_Value key_value = LT_VALUE_NULL;
        if (!json_string_to_little(vm, key, &key_value))
        {
            lt_pop(vm);
            return 0;
        }
        lt_push(vm, key_value);
        lt_Value child = LT_VALUE_NULL;
        if (!json_to_little(vm, yyjson_obj_iter_get_val(key), &child, depth + 1))
        {
            lt_pop(vm);
            lt_pop(vm);
            return 0;
        }
        lt_push(vm, child);
        lt_table_set(vm, table, key_value, child);
        lt_pop(vm);
        lt_pop(vm);
    }
    lt_pop(vm);
    *out = table;
    return 1;
}

static uint8_t json_to_little(lt_VM* vm, yyjson_val* value, lt_Value* out, uint8_t depth)
{
    if (depth > LITTLE_JSON_DEPTH_LIMIT) return 0;

    if (yyjson_is_null(value))
    {
        *out = LT_VALUE_NULL;
        return 1;
    }
    if (yyjson_is_bool(value))
    {
        *out = yyjson_get_bool(value) ? LT_VALUE_TRUE : LT_VALUE_FALSE;
        return 1;
    }
    if (yyjson_is_num(value))
    {
        *out = lt_make_number(yyjson_get_num(value));
        return 1;
    }
    if (yyjson_is_str(value)) return json_string_to_little(vm, value, out);
    if (yyjson_is_arr(value)) return json_array_to_little(vm, value, out, depth);
    if (yyjson_is_obj(value)) return json_object_to_little(vm, value, out, depth);
    return 0;
}

static yyjson_mut_val* little_to_json(lt_VM* vm, yyjson_mut_doc* doc, lt_Value value, uint8_t depth);

static yyjson_mut_val* little_array_to_json(lt_VM* vm, yyjson_mut_doc* doc, lt_Value value, uint8_t depth)
{
    yyjson_mut_val* array = yyjson_mut_arr(doc);
    if (!array) return 0;
    for (uint32_t i = 0; i < lt_array_length(value); ++i)
    {
        yyjson_mut_val* child = little_to_json(vm, doc, lt_array_get(vm, value, i), depth + 1);
        if (!child || !yyjson_mut_arr_append(array, child)) return 0;
    }
    return array;
}

static yyjson_mut_val* little_table_to_json(lt_VM* vm, yyjson_mut_doc* doc, lt_Value value, uint8_t depth)
{
    yyjson_mut_val* object = yyjson_mut_obj(doc);
    if (!object) return 0;
    uint64_t cursor = 0;
    lt_Value key = LT_VALUE_NULL;
    lt_Value child_value = LT_VALUE_NULL;
    while (lt_table_next(vm, value, &cursor, &key, &child_value))
    {
        if (!LT_IS_STRING(key)) return 0;
        yyjson_mut_val* key_value = yyjson_mut_strcpy(doc, lt_get_string(vm, key));
        yyjson_mut_val* child = little_to_json(vm, doc, child_value, depth + 1);
        if (!key_value || !child || !yyjson_mut_obj_add(object, key_value, child)) return 0;
    }
    return object;
}

static yyjson_mut_val* little_to_json(lt_VM* vm, yyjson_mut_doc* doc, lt_Value value, uint8_t depth)
{
    if (depth > LITTLE_JSON_DEPTH_LIMIT) return 0;
    if (LT_IS_NULL(value)) return yyjson_mut_null(doc);
    if (LT_IS_TRUE(value)) return yyjson_mut_true(doc);
    if (LT_IS_FALSE(value)) return yyjson_mut_false(doc);
    if (LT_IS_STRING(value)) return yyjson_mut_strcpy(doc, lt_get_string(vm, value));
    if (LT_IS_NUMBER(value))
    {
        double number = lt_get_number(value);
        if (!isfinite(number)) return yyjson_mut_null(doc);
        if (number >= (double)INT64_MIN && number < 9223372036854775808.0 && number == floor(number))
            return yyjson_mut_sint(doc, (int64_t)number);
        if (number >= 0 && number < 18446744073709551616.0 && number == floor(number))
            return yyjson_mut_uint(doc, (uint64_t)number);
        return yyjson_mut_real(doc, number);
    }
    if (LT_IS_ARRAY(value)) return little_array_to_json(vm, doc, value, depth);
    if (LT_IS_TABLE(value)) return little_table_to_json(vm, doc, value, depth);
    return 0;
}

uint8_t lt_json_parse(lt_VM* vm, const char* text, lt_Value* out)
{
    if (!text || !out) return 0;
    yyjson_read_err error;
    yyjson_doc* doc = yyjson_read_opts((char*)text, strlen(text), YYJSON_READ_NOFLAG, 0, &error);
    if (!doc) return 0;
    uint8_t ok = json_to_little(vm, yyjson_doc_get_root(doc), out, 0);
    yyjson_doc_free(doc);
    return ok;
}

lt_Value lt_json_stringify(lt_VM* vm, lt_Value value)
{
    yyjson_mut_doc* doc = yyjson_mut_doc_new(0);
    if (!doc) return LT_VALUE_NULL;
    yyjson_mut_val* root = little_to_json(vm, doc, value, 0);
    if (!root)
    {
        yyjson_mut_doc_free(doc);
        return LT_VALUE_NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, 0);
    yyjson_mut_doc_free(doc);
    if (!text) return LT_VALUE_NULL;
    lt_Value result = lt_make_string(vm, text);
    free(text);
    return result;
}

static uint8_t json_parse_native(lt_VM* vm, uint8_t argc)
{
    if (argc != 1)
    {
        while (argc--) lt_pop(vm);
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value input = vm->stack[vm->top - 1];
    if (!LT_IS_STRING(input))
    {
        lt_pop(vm);
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value value = LT_VALUE_NULL;
    uint8_t ok = lt_json_parse(vm, lt_get_string(vm, input), &value);
    lt_pop(vm);
    lt_push(vm, ok ? value : LT_VALUE_NULL);
    return 1;
}

static uint8_t json_stringify_native(lt_VM* vm, uint8_t argc)
{
    if (argc != 1)
    {
        while (argc--) lt_pop(vm);
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value value = vm->stack[vm->top - 1];
    lt_Value result = lt_json_stringify(vm, value);
    lt_pop(vm);
    lt_push(vm, result);
    return 1;
}

/**
 * Opens the built-in JSON module and registers it as the global `json` module.
 * @param vm Virtual machine used to load and register the JSON module.
 */
void ltstd_open_json(lt_VM* vm)
{
    lt_Value json = lt_make_table(vm);
    /** [LittleBinding] json.parse
     *  parses JSON text into a Little value, returning null when the text is not valid JSON.
     *  @signature (text)
     *  @param text JSON document to parse
     */
    lt_table_set(vm, json, lt_make_string(vm, "parse"), lt_make_native(vm, json_parse_native));
    /** [LittleBinding] json.stringify
     *  serializes a Little value as JSON text, returning null for values JSON cannot represent.
     *  @signature (value)
     *  @param value value to serialize
     */
    lt_table_set(vm, json, lt_make_string(vm, "stringify"), lt_make_native(vm, json_stringify_native));
    lt_table_set(vm, vm->global, lt_make_string(vm, "json"), json);
}
