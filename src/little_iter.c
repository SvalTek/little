#include "little.h"
#include "little_async_internal.h"
#include "little_runtime_internal.h"

static uint8_t _lt_iter_binding_count(lt_VM* vm)
{
	lt_Value bindings = lt_getupval(vm, 3);
	if (!LT_IS_NUMBER(bindings)) return 1;
	uint32_t count = (uint32_t)lt_get_number(bindings);
	return count == 2 ? 2 : 1;
}

static uint8_t _lt_array_iter_next(lt_VM* vm, uint8_t argc)
{
	if (argc != 0) lt_runtime_error(vm, "Array iterator expects no arguments!");
	lt_Value source = lt_getupval(vm, 0);
	lt_Value cursor_value = lt_getupval(vm, 2);
	uint32_t index = LT_IS_NUMBER(cursor_value) ? (uint32_t)lt_get_number(cursor_value) : 0;
	if (index >= lt_array_length(source))
	{
		lt_push(vm, LT_VALUE_NULL);
		return 1;
	}

	lt_Value value = lt_array_get(vm, source, index);
	lt_setupval(vm, 2, lt_make_number((double)index + 1));
	if (_lt_iter_binding_count(vm) == 1)
	{
		lt_push(vm, value);
		return 1;
	}

	lt_push(vm, lt_make_number(index));
	lt_push(vm, value);
	return 2;
}

static uint8_t _lt_table_iter_next(lt_VM* vm, uint8_t argc)
{
	if (argc != 0) lt_runtime_error(vm, "Table iterator expects no arguments!");
	lt_Value source = lt_getupval(vm, 0);
	lt_Value bucket_value = lt_getupval(vm, 1);
	lt_Value index_value = lt_getupval(vm, 2);
	uint64_t bucket = LT_IS_NUMBER(bucket_value) ? (uint64_t)lt_get_number(bucket_value) : 0;
	uint64_t index = LT_IS_NUMBER(index_value) ? (uint64_t)lt_get_number(index_value) : 0;
	uint64_t cursor = (bucket << 60) | index;
	lt_Value key = LT_VALUE_NULL;
	lt_Value value = LT_VALUE_NULL;

	while (lt_table_next(vm, source, &cursor, &key, &value))
	{
		if (LT_IS_NULL(key) || LT_IS_NULL(value)) continue;
		lt_setupval(vm, 1, lt_make_number((double)(cursor >> 60)));
		lt_setupval(vm, 2, lt_make_number((double)(cursor & (((uint64_t)1 << 60) - 1))));
		if (_lt_iter_binding_count(vm) == 1)
		{
			lt_push(vm, value);
			return 1;
		}

		lt_push(vm, key);
		lt_push(vm, value);
		return 2;
	}

	lt_setupval(vm, 1, lt_make_number((double)(cursor >> 60)));
	lt_setupval(vm, 2, lt_make_number((double)(cursor & (((uint64_t)1 << 60) - 1))));
	lt_push(vm, LT_VALUE_NULL);
	return 1;
}

lt_Value lt_make_iter_internal(lt_VM* vm, lt_Value source, uint8_t bindings)
{
	if (ltasync_is_callable(source) || LT_IS_CLASS(source)) return source;

	lt_NativeFn next = 0;
	if (LT_IS_ARRAY(source)) next = _lt_array_iter_next;
	else if (LT_IS_TABLE(source)) next = _lt_table_iter_next;
	else
	{
		lt_runtime_error(vm, "Expected array, table, or callable iterator for 'for'!");
		return LT_VALUE_NULL;
	}

	lt_push(vm, lt_make_native(vm, next));
	lt_push(vm, lt_make_number(bindings));
	lt_push(vm, LT_VALUE_NUMBER(0));
	lt_push(vm, LT_VALUE_NUMBER(0));
	lt_push(vm, source);
	lt_close(vm, 4);
	return lt_pop(vm);
}

static lt_Value _lt_iter_member(lt_VM* vm, lt_Value source, const char* name)
{
	lt_Value key = lt_make_string(vm, name);
	if (LT_IS_TABLE(source)) return lt_table_get(vm, source, key);
	if (LT_IS_INSTANCE(source)) return lt_instance_get_internal(vm, source, key);
	return LT_VALUE_NULL;
}

static uint8_t _lt_async_next_dispatch(lt_VM* vm, uint8_t argc)
{
	if (argc != 0) lt_runtime_error(vm, "Async iterator next expects no arguments!");
	lt_Value source = lt_getupval(vm, 0);
	lt_Value next = lt_getupval(vm, 1);
	lt_push(vm, source);
	if (ltasync_is_async_callable(next))
	{
		lt_Value promise = ltasync_call(vm, next, 1);
		lt_push(vm, promise);
		return 1;
	}
	return lt_exec_internal(vm, next, 1);
}

lt_Value lt_make_async_next_internal(lt_VM* vm, lt_Value source)
{
	if (ltasync_is_callable(source)) return source;
	if (!LT_IS_TABLE(source) && !LT_IS_INSTANCE(source))
		lt_runtime_error(vm, "Expected async iterator callable or object with next!");

	lt_Value next = _lt_iter_member(vm, source, "next");
	if (!ltasync_is_callable(next))
		lt_runtime_error(vm, "Expected async iterator next to be callable!");

	lt_push(vm, lt_make_native(vm, _lt_async_next_dispatch));
	lt_push(vm, next);
	lt_push(vm, source);
	lt_close(vm, 2);
	return lt_pop(vm);
}

void lt_close_async_source_internal(lt_VM* vm, lt_Value source)
{
	if (ltasync_is_callable(source)) return;
	if (!LT_IS_TABLE(source) && !LT_IS_INSTANCE(source)) return;

	lt_Value close = _lt_iter_member(vm, source, "close");
	if (LT_IS_NULL(close)) return;
	if (!ltasync_is_callable(close))
		lt_runtime_error(vm, "Expected async iterator close to be callable!");

	lt_push(vm, source);
	if (ltasync_is_async_callable(close))
	{
		lt_Value promise = ltasync_call(vm, close, 1);
		ltasync_await(vm, promise);
		return;
	}

	uint16_t nret = lt_exec_internal(vm, close, 1);
	if (nret == 1)
	{
		lt_Value result = lt_pop(vm);
		if (lt_native_is_promise_internal(result)) ltasync_await(vm, result);
	}
	else while (nret-- > 0) lt_pop(vm);
}
