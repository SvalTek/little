#include "little.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"
#include "little_shared_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static lt_Value LT_NULL = LT_VALUE_NULL;
#define HASH_VALUE(x) (LT_IS_OBJECT(x) ? (((x) >> 2) % 16) : ((x) % 16))

void lt_table_init_internal(lt_Table* table)
{
	memset(table, 0, sizeof(lt_Table));
}

void lt_table_destroy_internal(lt_VM* vm, lt_Table* table)
{
	for (uint8_t i = 0; i < 16; ++i)
		lt_buffer_destroy(vm, table->buckets + i);
}

void lt_table_mark_internal(lt_VM* vm, lt_Table* table)
{
	for (uint16_t i = 0; i < 16; ++i)
	{
		lt_Buffer* bucket = table->buckets + i;
		for (uint32_t j = 0; j < bucket->length; ++j)
		{
			lt_TablePair* pair = lt_buffer_at(bucket, j);
			lt_sweep_v(vm, pair->key);
			lt_sweep_v(vm, pair->value);
		}
	}
}

lt_TablePair* lt_table_index_raw_internal(lt_VM* vm, lt_Table* table, lt_Value key, uint8_t alloc)
{
	uint8_t bucket_idx = HASH_VALUE(key);
	lt_Buffer* bucket = table->buckets + bucket_idx;
	if (alloc && bucket->element_size == 0) *bucket = lt_buffer_new(sizeof(lt_TablePair));

	for (uint32_t i = 0; i < bucket->length; i++)
	{
		lt_TablePair* pair = lt_buffer_at(bucket, i);
		if (lt_equals(pair->key, key)) return pair;
	}

	return 0;
}

lt_Value lt_table_get_raw_internal(lt_Table* table, lt_Value key)
{
	lt_TablePair* pair = lt_table_index_raw_internal(0, table, key, 0);
	return pair ? pair->value : LT_VALUE_NULL;
}

/* Native functions and tables are anonymous objects. Take a debug label from the
   first string key the value is stored under, so tracebacks can name natives; a
   native inside a named table is qualified with that table's label. */
static uint8_t _lt_is_plain_identifier(const char* text)
{
	if (!text || !(isalpha((unsigned char)text[0]) || text[0] == '_')) return 0;
	for (const unsigned char* c = (const unsigned char*)text + 1; *c; ++c)
		if (!(isalnum(*c) || *c == '_')) return 0;
	return 1;
}

/* "parent.key" when both are plain identifiers and it fits, otherwise the key. */
static lt_Value _lt_qualified_name(lt_VM* vm, lt_Value parent_value, lt_Value key)
{
	if (!LT_IS_STRING(parent_value) || !LT_IS_STRING(key)) return key;
	const char* parent = lt_get_string(vm, parent_value);
	const char* key_text = lt_get_string(vm, key);
	char qualified[192];
	if (!_lt_is_plain_identifier(parent)) return key;
	if (strlen(parent) + strlen(key_text) + 2 > sizeof(qualified)) return key;
	snprintf(qualified, sizeof(qualified), "%s.%s", parent, key_text);
	return lt_make_string(vm, qualified);
}

/* A table that just got a name may already hold natives labelled with their bare
   key; qualify those, since the table is named only once it is registered. */
static void _lt_qualify_table_natives(lt_VM* vm, lt_Object* table_obj)
{
	for (uint16_t i = 0; i < 16; ++i)
	{
		lt_Buffer* bucket = table_obj->table.buckets + i;
		for (uint32_t j = 0; j < bucket->length; ++j)
		{
			lt_TablePair* pair = lt_buffer_at(bucket, j);
			if (!LT_IS_STRING(pair->key) || !LT_IS_OBJECT(pair->value)) continue;
			lt_Object* native = lt_native_callee_internal(LT_GET_OBJECT(pair->value));
			if (!native) continue;
			if (!LT_IS_STRING(native->debug_name) || native->debug_name != pair->key) continue;
			native->debug_name = _lt_qualified_name(vm, table_obj->debug_name, pair->key);
		}
	}
}

void lt_name_value_internal(lt_VM* vm, lt_Value parent_name, lt_Value key, lt_Value val)
{
	if (!LT_IS_STRING(key) || !LT_IS_OBJECT(val)) return;
	lt_Object* obj = LT_GET_OBJECT(val);
	lt_Object* native = lt_native_callee_internal(obj);
	if (!native && obj->type != LT_OBJECT_TABLE) return;
	if (!LT_IS_NULL((native ? native : obj)->debug_name)) return;

	if (native)
	{
		native->debug_name = _lt_qualified_name(vm, parent_name, key);
		return;
	}

	obj->debug_name = key;
	_lt_qualify_table_natives(vm, obj);
}

lt_Value lt_table_set_raw_internal(lt_VM* vm, lt_Table* table, lt_Value key, lt_Value val)
{
	lt_name_value_internal(vm, LT_VALUE_NULL, key, val);
	lt_TablePair* pair = lt_table_index_raw_internal(vm, table, key, 1);
	if (pair)
	{
		pair->value = val;
		return val;
	}

	lt_Buffer* bucket = table->buckets + HASH_VALUE(key);
	lt_TablePair newpair = { key, val };
	lt_buffer_push(vm, bucket, &newpair);
	return val;
}

#define HASH(x) (LT_IS_OBJECT(x) ? ((x >> 2) % 16) : (x % 16))

lt_TablePair* lt_table_index_internal(lt_VM* vm, lt_Value table, lt_Value key, uint8_t alloc)
{
	if (!LT_IS_OBJECT(table) || LT_GET_OBJECT(table)->type != LT_OBJECT_TABLE) return 0;
	uint8_t bucket = HASH(key);
	lt_Buffer* buf = LT_GET_OBJECT(table)->table.buckets + bucket;
	if (alloc && buf->element_size == 0) *buf = lt_buffer_new(sizeof(lt_TablePair));

	for (uint32_t i = 0; i < buf->length; i++)
	{
		lt_TablePair* p = lt_buffer_at(buf, i);
		if (lt_equals(p->key, key))
		{
			return p;
		}
	}

	return 0;
}

lt_Value lt_make_table(lt_VM* vm)
{
	return LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_TABLE));
}

lt_Value lt_table_set(lt_VM* vm, lt_Value table, lt_Value key, lt_Value val)
{
	if (!LT_IS_TABLE(table)) return LT_VALUE_NULL;
	lt_name_value_internal(vm, LT_GET_OBJECT(table)->debug_name, key, val);
	if (LT_GET_OBJECT(table)->type == LT_OBJECT_SHARED_TABLE)
		return ltshared_table_set(vm, LT_GET_OBJECT(table)->shared, key, val);
	lt_TablePair* p = lt_table_index_internal(vm, table, key, 1);
	if (p)
	{
		p->value = val;
		return val;
	}

	uint8_t bucket = HASH(key);
	lt_Buffer* buf = LT_GET_OBJECT(table)->table.buckets + bucket;
	lt_TablePair newpair = { key, val };
	lt_buffer_push(vm, buf, &newpair);
	return val;
}

lt_Value lt_table_get(lt_VM* vm, lt_Value table, lt_Value key)
{
	if (!LT_IS_TABLE(table)) return LT_VALUE_NULL;
	if (LT_GET_OBJECT(table)->type == LT_OBJECT_SHARED_TABLE)
		return ltshared_table_get(vm, LT_GET_OBJECT(table)->shared, key);
	lt_TablePair* p = lt_table_index_internal(vm, table, key, 0);
	if (p) return p->value;
	return LT_VALUE_NULL;
}

uint8_t lt_table_next(lt_VM* vm, lt_Value table, uint64_t* cursor, lt_Value* key, lt_Value* val)
{
	/* Four bits select one of 16 buckets; the remaining 60 bits hold the pair index. */
	enum {
		LT_TABLE_CURSOR_BUCKET_BITS = 4,
		LT_TABLE_CURSOR_INDEX_BITS = 64 - LT_TABLE_CURSOR_BUCKET_BITS,
		LT_TABLE_CURSOR_BUCKET_COUNT = 1 << LT_TABLE_CURSOR_BUCKET_BITS
	};
	const uint64_t table_cursor_index_mask = (UINT64_C(1) << LT_TABLE_CURSOR_INDEX_BITS) - 1;

	if (!LT_IS_TABLE(table)) return 0;
	lt_Object* obj = LT_GET_OBJECT(table);
	if (obj->type == LT_OBJECT_SHARED_TABLE)
		return ltshared_table_next(vm, obj->shared, cursor, key, val);
	if (obj->type != LT_OBJECT_TABLE) return 0;

	uint64_t bucket = *cursor >> LT_TABLE_CURSOR_INDEX_BITS;
	uint64_t index = *cursor & table_cursor_index_mask;

	for (; bucket < LT_TABLE_CURSOR_BUCKET_COUNT; ++bucket)
	{
		lt_Buffer* buf = obj->table.buckets + bucket;
		if (index < buf->length)
		{
			lt_TablePair* pair = lt_buffer_at(buf, index);
			*key = pair->key;
			*val = pair->value;
			*cursor = (bucket << LT_TABLE_CURSOR_INDEX_BITS) | (index + 1);
			return 1;
		}
		index = 0;
	}

	return 0;
}

uint8_t lt_table_pop(lt_VM* vm, lt_Value table, lt_Value key)
{
	return lt_table_set(vm, table, key, LT_VALUE_NULL) == LT_VALUE_NULL;
}

lt_Value lt_make_array(lt_VM* vm)
{
	return LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_ARRAY));
}

lt_Value lt_array_push(lt_VM* vm, lt_Value array, lt_Value val)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_push(vm, arr->shared, val);
	if (arr->array.element_size == 0) arr->array = lt_buffer_new(sizeof(lt_Value));
	lt_buffer_push(vm, &arr->array, &val);
	return val;
}

lt_Value lt_array_get(lt_VM* vm, lt_Value array, uint32_t idx)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_get(vm, arr->shared, idx);
	return idx < arr->array.length ? *(lt_Value*)lt_buffer_at(&arr->array, idx) : LT_VALUE_NULL;
}

lt_Value lt_array_set(lt_VM* vm, lt_Value array, uint32_t idx, lt_Value val)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_set(vm, arr->shared, idx, val);
	if (idx >= arr->array.length) return LT_VALUE_NULL;
	*(lt_Value*)lt_buffer_at(&arr->array, idx) = val;
	return val;
}

lt_Value* lt_array_at(lt_Value array, uint32_t idx)
{
	if (!LT_IS_ARRAY(array) || LT_GET_OBJECT(array)->type == LT_OBJECT_SHARED_ARRAY) return &LT_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	return lt_buffer_at(&arr->array, idx);
}

lt_Value lt_array_remove(lt_VM* vm, lt_Value array, uint32_t idx)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_remove(vm, arr->shared, idx);
	lt_Value old = *(lt_Value*)lt_buffer_at(&arr->array, idx);
	lt_buffer_cycle(&arr->array, idx);
	return old;
}

uint32_t lt_array_length(lt_Value array)
{
	if (!LT_IS_ARRAY(array)) return 0;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_length(arr->shared);
	return arr->array.length;
}
