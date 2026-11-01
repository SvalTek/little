#include "little.h"
#include "little_buffer_internal.h"
#include "little_async_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lt_SharedObject* lt_shared_registry = 0;

#if defined(_WIN32)
static CRITICAL_SECTION lt_shared_registry_mutex;
static volatile LONG lt_shared_registry_init = 0;

static void _lt_shared_registry_lock(void)
{
	if (lt_shared_registry_init != 2)
	{
		if (InterlockedCompareExchange(&lt_shared_registry_init, 1, 0) == 0)
		{
			InitializeCriticalSection(&lt_shared_registry_mutex);
			InterlockedExchange(&lt_shared_registry_init, 2);
		}
		else
		{
			while (lt_shared_registry_init != 2) Sleep(0);
		}
	}
	EnterCriticalSection(&lt_shared_registry_mutex);
}

static void _lt_shared_registry_unlock(void)
{
	LeaveCriticalSection(&lt_shared_registry_mutex);
}
#else
static pthread_mutex_t lt_shared_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static void _lt_shared_registry_lock(void)
{
	pthread_mutex_lock(&lt_shared_registry_mutex);
}

static void _lt_shared_registry_unlock(void)
{
	pthread_mutex_unlock(&lt_shared_registry_mutex);
}
#endif

static void _lt_shared_lock(lt_SharedObject* shared)
{
#if defined(_WIN32)
	EnterCriticalSection(&shared->lock);
#else
	pthread_mutex_lock(&shared->lock);
#endif
}

static void _lt_shared_unlock(lt_SharedObject* shared)
{
#if defined(_WIN32)
	LeaveCriticalSection(&shared->lock);
#else
	pthread_mutex_unlock(&shared->lock);
#endif
}

static lt_SharedObject* _lt_shared_new(lt_SharedKind kind)
{
	lt_SharedObject* shared = malloc(sizeof(lt_SharedObject));
	if (!shared) return 0;
	memset(shared, 0, sizeof(lt_SharedObject));
	shared->kind = kind;
#if defined(_WIN32)
	InitializeCriticalSection(&shared->lock);
#else
	pthread_mutex_init(&shared->lock, 0);
#endif
	_lt_shared_registry_lock();
	shared->next = lt_shared_registry;
	lt_shared_registry = shared;
	_lt_shared_registry_unlock();
	return shared;
}

static char* _lt_shared_strdup(const char* string);
static void _lt_shared_collect(void);
static void _lt_shared_unregister(lt_SharedObject* shared);
static void _lt_shared_destroy_unregistered(lt_SharedObject* shared);

void ltshared_retain(lt_SharedObject* shared)
{
	if (!shared) return;
#if defined(_WIN32)
	InterlockedIncrement((volatile LONG*)&shared->external_refs);
#else
	__sync_add_and_fetch(&shared->external_refs, 1);
#endif
}

static uint32_t _lt_shared_external_refs_load(lt_SharedObject* shared)
{
#if defined(_WIN32)
	return (uint32_t)InterlockedCompareExchange((volatile LONG*)&shared->external_refs, 0, 0);
#else
	return __sync_add_and_fetch(&shared->external_refs, 0);
#endif
}

void lt_shared_value_clear_internal(lt_SharedValue* value)
{
	if (value->type == LT_SHARED_VALUE_STRING && value->string) free(value->string);
	memset(value, 0, sizeof(lt_SharedValue));
}

static uint8_t _lt_shared_value_clone(lt_SharedValue* dst, lt_SharedValue* src)
{
	memset(dst, 0, sizeof(lt_SharedValue));
	dst->type = src->type;
	switch (src->type)
	{
	case LT_SHARED_VALUE_STRING:
		dst->string = _lt_shared_strdup(src->string);
		return dst->string != 0;
	case LT_SHARED_VALUE_OBJECT:
		dst->object = src->object;
		return 1;
	case LT_SHARED_VALUE_BOOL:
		dst->boolean = src->boolean;
		return 1;
	case LT_SHARED_VALUE_NUMBER:
		dst->number = src->number;
		return 1;
	case LT_SHARED_VALUE_NULL:
		return 1;
	}
	return 0;
}

void lt_shared_value_retain_external_internal(lt_SharedValue* value)
{
	if (value->type == LT_SHARED_VALUE_OBJECT) ltshared_retain(value->object);
}

void lt_shared_value_release_external_internal(lt_SharedValue* value)
{
	if (value->type == LT_SHARED_VALUE_OBJECT) ltshared_release(value->object);
}

static void _lt_shared_destroy_unregistered(lt_SharedObject* shared)
{
	if (!shared) return;
	if (shared->kind == LT_SHARED_TABLE)
	{
		for (uint32_t i = 0; i < shared->table.length; ++i)
		{
			lt_shared_value_clear_internal(&shared->table.pairs[i].key);
			lt_shared_value_clear_internal(&shared->table.pairs[i].value);
		}
		free(shared->table.pairs);
	}
	else
	{
		for (uint32_t i = 0; i < shared->array.length; ++i)
			lt_shared_value_clear_internal(&shared->array.values[i]);
		free(shared->array.values);
	}

#if defined(_WIN32)
	DeleteCriticalSection(&shared->lock);
#else
	pthread_mutex_destroy(&shared->lock);
#endif
	free(shared);
}

static void _lt_shared_unregister(lt_SharedObject* shared)
{
	lt_SharedObject** current = &lt_shared_registry;
	while (*current)
	{
		if (*current == shared)
		{
			*current = shared->next;
			shared->next = 0;
			return;
		}
		current = &(*current)->next;
	}
}

static uint8_t _lt_shared_mark(lt_SharedObject* shared, uint32_t* count)
{
	if (!shared || shared->gc_mark) return 1;
	if (++(*count) > LT_TASK_SHARED_MAX_NODES) return 0;

	shared->gc_mark = 1;
	_lt_shared_lock(shared);
	if (shared->kind == LT_SHARED_TABLE)
	{
		for (uint32_t i = 0; i < shared->table.length; ++i)
		{
			if (shared->table.pairs[i].key.type == LT_SHARED_VALUE_OBJECT &&
				!_lt_shared_mark(shared->table.pairs[i].key.object, count))
			{
				_lt_shared_unlock(shared);
				return 0;
			}
			if (shared->table.pairs[i].value.type == LT_SHARED_VALUE_OBJECT &&
				!_lt_shared_mark(shared->table.pairs[i].value.object, count))
			{
				_lt_shared_unlock(shared);
				return 0;
			}
		}
	}
	else
	{
		for (uint32_t i = 0; i < shared->array.length; ++i)
		{
			if (shared->array.values[i].type == LT_SHARED_VALUE_OBJECT &&
				!_lt_shared_mark(shared->array.values[i].object, count))
			{
				_lt_shared_unlock(shared);
				return 0;
			}
		}
	}
	_lt_shared_unlock(shared);
	return 1;
}

static void _lt_shared_collect(void)
{
	_lt_shared_registry_lock();
	for (lt_SharedObject* current = lt_shared_registry; current; current = current->next)
		current->gc_mark = 0;

	uint32_t mark_count = 0;
	for (lt_SharedObject* current = lt_shared_registry; current; current = current->next)
	{
		uint32_t external_refs = _lt_shared_external_refs_load(current);
		if (external_refs > 0 && !_lt_shared_mark(current, &mark_count))
		{
			for (lt_SharedObject* clear = lt_shared_registry; clear; clear = clear->next)
				clear->gc_mark = 0;
			_lt_shared_registry_unlock();
			return;
		}
	}

	lt_SharedObject* to_free = 0;
	lt_SharedObject** current = &lt_shared_registry;
	while (*current)
	{
		lt_SharedObject* shared = *current;
		if (!shared->gc_mark)
		{
			*current = shared->next;
			shared->next = to_free;
			to_free = shared;
		}
		else current = &shared->next;
	}
	_lt_shared_registry_unlock();

	while (to_free)
	{
		lt_SharedObject* next = to_free->next;
		to_free->next = 0;
		_lt_shared_destroy_unregistered(to_free);
		to_free = next;
	}
}

void ltshared_release(lt_SharedObject* shared)
{
	if (!shared) return;
#if defined(_WIN32)
	LONG refs = InterlockedDecrement((volatile LONG*)&shared->external_refs);
#else
	uint32_t refs = __sync_sub_and_fetch(&shared->external_refs, 1);
#endif
	if (refs == 0) _lt_shared_collect();
}

static char* _lt_shared_strdup(const char* string)
{
	uint32_t len = (uint32_t)strlen(string);
	char* copy = malloc(len + 1);
	if (!copy) return 0;
	memcpy(copy, string, len + 1);
	return copy;
}

uint8_t lt_shared_reserve_internal(void** data, uint32_t* capacity, uint32_t length, uint32_t element_size)
{
	if (length < *capacity) return 1;
	if (*capacity > UINT32_MAX - 16) return 0;
	uint32_t next_capacity = *capacity + 16;
	if (element_size != 0 && next_capacity > UINT32_MAX / element_size) return 0;
	size_t required_bytes = (size_t)next_capacity * (size_t)element_size;
	void* next = realloc(*data, required_bytes);
	if (!next) return 0;
	*data = next;
	*capacity = next_capacity;
	return 1;
}

static uint8_t _lt_shared_value_equals(lt_SharedValue* a, lt_SharedValue* b)
{
	if (a->type != b->type) return 0;
	switch (a->type)
	{
	case LT_SHARED_VALUE_NULL: return 1;
	case LT_SHARED_VALUE_BOOL: return a->boolean == b->boolean;
	case LT_SHARED_VALUE_NUMBER: return a->number == b->number;
	case LT_SHARED_VALUE_STRING: return strcmp(a->string, b->string) == 0;
	case LT_SHARED_VALUE_OBJECT: return a->object == b->object;
	}
	return 0;
}

static lt_SharedObject* _lt_shared_ctx_find(lt_SharedMarshalCtx* ctx, lt_Object* object)
{
	for (uint32_t i = 0; i < ctx->length; ++i)
		if (ctx->entries[i].object == object) return ctx->entries[i].shared;
	return 0;
}

static uint8_t _lt_shared_ctx_add(lt_SharedMarshalCtx* ctx, lt_Object* object, lt_SharedObject* shared)
{
	if (!lt_shared_reserve_internal((void**)&ctx->entries, &ctx->capacity, ctx->length, sizeof(lt_SharedMarshalEntry))) return 0;
	ctx->entries[ctx->length++] = (lt_SharedMarshalEntry){ object, shared };
	return 1;
}

void lt_shared_ctx_destroy_internal(lt_SharedMarshalCtx* ctx)
{
	free(ctx->entries);
	memset(ctx, 0, sizeof(lt_SharedMarshalCtx));
}

uint8_t lt_shared_from_vm_internal(lt_SharedMarshalCtx* ctx, lt_VM* vm, lt_Value value, lt_SharedValue* out);

static lt_SharedObject* _lt_shared_promote_object(lt_SharedMarshalCtx* ctx, lt_VM* vm, lt_Object* object)
{
	if (object->type == LT_OBJECT_SHARED_TABLE || object->type == LT_OBJECT_SHARED_ARRAY) return object->shared;

	lt_SharedObject* existing = _lt_shared_ctx_find(ctx, object);
	if (existing) return existing;
	if (ctx->length >= LT_TASK_SHARED_MAX_NODES)
	{
		snprintf(ctx->error, sizeof(ctx->error), "Task shared state exceeds maximum object count!");
		return 0;
	}

	lt_SharedKind kind = object->type == LT_OBJECT_TABLE ? LT_SHARED_TABLE : LT_SHARED_ARRAY;
	lt_SharedObject* shared = _lt_shared_new(kind);
	if (!shared)
	{
		snprintf(ctx->error, sizeof(ctx->error), "Failed to allocate shared task state!");
		return 0;
	}
	if (!_lt_shared_ctx_add(ctx, object, shared))
	{
		snprintf(ctx->error, sizeof(ctx->error), "Failed to track shared task state!");
		_lt_shared_registry_lock();
		_lt_shared_unregister(shared);
		_lt_shared_registry_unlock();
		_lt_shared_destroy_unregistered(shared);
		return 0;
	}

	if (object->type == LT_OBJECT_TABLE)
	{
		for (uint16_t i = 0; i < 16; ++i)
		{
			lt_Buffer* bucket = object->table.buckets + i;
			for (uint32_t j = 0; j < bucket->length; ++j)
			{
				lt_TablePair* pair = lt_buffer_at(bucket, j);
				if (!lt_shared_reserve_internal((void**)&shared->table.pairs, &shared->table.capacity, shared->table.length, sizeof(lt_SharedPair)))
				{
					snprintf(ctx->error, sizeof(ctx->error), "Failed to grow shared table!");
					return 0;
				}

				lt_SharedPair* next = &shared->table.pairs[shared->table.length];
				memset(next, 0, sizeof(lt_SharedPair));
				if (!lt_shared_from_vm_internal(ctx, vm, pair->key, &next->key)) return 0;
				if (!lt_shared_from_vm_internal(ctx, vm, pair->value, &next->value))
				{
					lt_shared_value_clear_internal(&next->key);
					return 0;
				}
				shared->table.length++;
			}
		}
		for (uint8_t i = 0; i < 16; ++i)
			lt_buffer_destroy(vm, object->table.buckets + i);
		object->type = LT_OBJECT_SHARED_TABLE;
	}
	else if (object->type == LT_OBJECT_ARRAY)
	{
		for (uint32_t i = 0; i < object->array.length; ++i)
		{
			if (!lt_shared_reserve_internal((void**)&shared->array.values, &shared->array.capacity, shared->array.length, sizeof(lt_SharedValue)))
			{
				snprintf(ctx->error, sizeof(ctx->error), "Failed to grow shared array!");
				return 0;
			}
			lt_SharedValue* next = &shared->array.values[shared->array.length];
			memset(next, 0, sizeof(lt_SharedValue));
			if (!lt_shared_from_vm_internal(ctx, vm, *(lt_Value*)lt_buffer_at(&object->array, i), next)) return 0;
			shared->array.length++;
		}
		lt_buffer_destroy(vm, &object->array);
		object->type = LT_OBJECT_SHARED_ARRAY;
	}
	else
	{
		snprintf(ctx->error, sizeof(ctx->error), "Unsupported value crossing worker boundary!");
		return 0;
	}

	object->shared = shared;
	ltshared_retain(shared);
	return shared;
}

uint8_t lt_shared_from_vm_internal(lt_SharedMarshalCtx* ctx, lt_VM* vm, lt_Value value, lt_SharedValue* out)
{
	memset(out, 0, sizeof(lt_SharedValue));
	if (LT_IS_NULL(value))
	{
		out->type = LT_SHARED_VALUE_NULL;
		return 1;
	}
	if (LT_IS_BOOL(value))
	{
		out->type = LT_SHARED_VALUE_BOOL;
		out->boolean = LT_IS_TRUE(value);
		return 1;
	}
	if (LT_IS_NUMBER(value))
	{
		out->type = LT_SHARED_VALUE_NUMBER;
		out->number = lt_get_number(value);
		return 1;
	}
	if (LT_IS_STRING(value))
	{
		out->type = LT_SHARED_VALUE_STRING;
		out->string = _lt_shared_strdup(lt_get_string(vm, value));
		if (!out->string)
		{
			snprintf(ctx->error, sizeof(ctx->error), "Failed to allocate shared string!");
			return 0;
		}
		return 1;
	}
	if (LT_IS_OBJECT(value))
	{
		lt_Object* object = LT_GET_OBJECT(value);
		if (object->type == LT_OBJECT_TABLE || object->type == LT_OBJECT_ARRAY ||
			object->type == LT_OBJECT_SHARED_TABLE || object->type == LT_OBJECT_SHARED_ARRAY)
		{
			if (ctx->depth >= LT_TASK_SHARED_MAX_DEPTH)
			{
				snprintf(ctx->error, sizeof(ctx->error), "Task shared state exceeds maximum nesting depth!");
				return 0;
			}
			ctx->depth++;
			lt_SharedObject* shared = _lt_shared_promote_object(ctx, vm, object);
			ctx->depth--;
			if (!shared) return 0;
			out->type = LT_SHARED_VALUE_OBJECT;
			out->object = shared;
			return 1;
		}
	}

	snprintf(ctx->error, sizeof(ctx->error), "Unsupported value crossing worker boundary!");
	return 0;
}

lt_Value lt_shared_to_vm_internal(lt_VM* vm, lt_SharedValue* value)
{
	switch (value->type)
	{
	case LT_SHARED_VALUE_NULL: return LT_VALUE_NULL;
	case LT_SHARED_VALUE_BOOL: return value->boolean ? LT_VALUE_TRUE : LT_VALUE_FALSE;
	case LT_SHARED_VALUE_NUMBER: return lt_make_number(value->number);
	case LT_SHARED_VALUE_STRING: return lt_make_string(vm, value->string);
	case LT_SHARED_VALUE_OBJECT: return ltshared_make_proxy(vm, value->object);
	}
	return LT_VALUE_NULL;
}

lt_Value ltshared_make_proxy(lt_VM* vm, lt_SharedObject* shared)
{
	lt_ObjectType type = shared->kind == LT_SHARED_TABLE ? LT_OBJECT_SHARED_TABLE : LT_OBJECT_SHARED_ARRAY;
	lt_Object* proxy = lt_allocate(vm, type);
	proxy->shared = shared;
	ltshared_retain(shared);
	return LT_VALUE_OBJECT(proxy);
}

lt_Value ltshared_table_get(lt_VM* vm, lt_SharedObject* shared, lt_Value key)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_key;
	if (!lt_shared_from_vm_internal(&ctx, vm, key, &shared_key))
	{
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}

	_lt_shared_lock(shared);
	for (uint32_t i = 0; i < shared->table.length; ++i)
	{
		if (_lt_shared_value_equals(&shared->table.pairs[i].key, &shared_key))
		{
			lt_SharedValue found;
			if (!_lt_shared_value_clone(&found, &shared->table.pairs[i].value))
			{
				_lt_shared_unlock(shared);
				lt_shared_value_clear_internal(&shared_key);
				lt_shared_ctx_destroy_internal(&ctx);
				return LT_VALUE_NULL;
			}
			_lt_shared_unlock(shared);
			lt_Value value = lt_shared_to_vm_internal(vm, &found);
			lt_shared_value_clear_internal(&found);
			lt_shared_value_clear_internal(&shared_key);
			lt_shared_ctx_destroy_internal(&ctx);
			return value;
		}
	}
	_lt_shared_unlock(shared);
	lt_shared_value_clear_internal(&shared_key);
	lt_shared_ctx_destroy_internal(&ctx);
	return LT_VALUE_NULL;
}

lt_Value ltshared_table_set(lt_VM* vm, lt_SharedObject* shared, lt_Value key, lt_Value val)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_key, shared_value;
	memset(&shared_key, 0, sizeof(shared_key));
	memset(&shared_value, 0, sizeof(shared_value));
	if (!lt_shared_from_vm_internal(&ctx, vm, key, &shared_key) || !lt_shared_from_vm_internal(&ctx, vm, val, &shared_value))
	{
		lt_shared_value_clear_internal(&shared_key);
		lt_shared_value_clear_internal(&shared_value);
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}

	_lt_shared_lock(shared);
	for (uint32_t i = 0; i < shared->table.length; ++i)
	{
		if (_lt_shared_value_equals(&shared->table.pairs[i].key, &shared_key))
		{
			lt_SharedValue old = shared->table.pairs[i].value;
			shared->table.pairs[i].value = shared_value;
			_lt_shared_unlock(shared);
			lt_shared_value_clear_internal(&old);
			lt_shared_value_clear_internal(&shared_key);
			lt_shared_ctx_destroy_internal(&ctx);
			return val;
		}
	}

	if (!lt_shared_reserve_internal((void**)&shared->table.pairs, &shared->table.capacity, shared->table.length, sizeof(lt_SharedPair)))
	{
		_lt_shared_unlock(shared);
		lt_shared_value_clear_internal(&shared_key);
		lt_shared_value_clear_internal(&shared_value);
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}
	shared->table.pairs[shared->table.length++] = (lt_SharedPair){ shared_key, shared_value };
	_lt_shared_unlock(shared);
	lt_shared_ctx_destroy_internal(&ctx);
	return val;
}

uint8_t ltshared_table_next(lt_VM* vm, lt_SharedObject* shared, uint64_t* cursor, lt_Value* key, lt_Value* val)
{
	lt_SharedValue shared_key;
	lt_SharedValue shared_value;
	memset(&shared_key, 0, sizeof(shared_key));
	memset(&shared_value, 0, sizeof(shared_value));

	_lt_shared_lock(shared);
	while (*cursor < shared->table.length)
	{
		lt_SharedPair* pair = &shared->table.pairs[(*cursor)++];
		if (pair->value.type == LT_SHARED_VALUE_NULL) continue;
		if (!_lt_shared_value_clone(&shared_key, &pair->key) ||
			!_lt_shared_value_clone(&shared_value, &pair->value))
		{
			lt_shared_value_clear_internal(&shared_key);
			lt_shared_value_clear_internal(&shared_value);
			_lt_shared_unlock(shared);
			return 0;
		}
		lt_shared_value_retain_external_internal(&shared_key);
		lt_shared_value_retain_external_internal(&shared_value);
		_lt_shared_unlock(shared);

		*key = lt_shared_to_vm_internal(vm, &shared_key);
		*val = lt_shared_to_vm_internal(vm, &shared_value);
		lt_shared_value_release_external_internal(&shared_key);
		lt_shared_value_release_external_internal(&shared_value);
		lt_shared_value_clear_internal(&shared_key);
		lt_shared_value_clear_internal(&shared_value);
		return 1;
	}
	_lt_shared_unlock(shared);
	return 0;
}

static lt_Value _lt_shared_table_collect_values(lt_VM* vm, lt_SharedObject* shared, uint8_t keys)
{
	lt_SharedValue* snapshot = 0;
	uint32_t length = 0;
	uint32_t capacity = 0;

	_lt_shared_lock(shared);
	for (uint32_t i = 0; i < shared->table.length; ++i)
	{
		if (shared->table.pairs[i].value.type == LT_SHARED_VALUE_NULL) continue;
		if (!lt_shared_reserve_internal((void**)&snapshot, &capacity, length, sizeof(lt_SharedValue)))
		{
			_lt_shared_unlock(shared);
			for (uint32_t j = 0; j < length; ++j)
			{
				lt_shared_value_release_external_internal(&snapshot[j]);
				lt_shared_value_clear_internal(&snapshot[j]);
			}
			free(snapshot);
			return lt_make_array(vm);
		}
		if (!_lt_shared_value_clone(&snapshot[length], keys ? &shared->table.pairs[i].key : &shared->table.pairs[i].value))
		{
			_lt_shared_unlock(shared);
			for (uint32_t j = 0; j < length; ++j)
			{
				lt_shared_value_release_external_internal(&snapshot[j]);
				lt_shared_value_clear_internal(&snapshot[j]);
			}
			free(snapshot);
			return lt_make_array(vm);
		}
		lt_shared_value_retain_external_internal(&snapshot[length]);
		length++;
	}
	_lt_shared_unlock(shared);

	lt_Value array = lt_make_array(vm);
	for (uint32_t i = 0; i < length; ++i)
	{
		lt_array_push(vm, array, lt_shared_to_vm_internal(vm, &snapshot[i]));
		lt_shared_value_release_external_internal(&snapshot[i]);
		lt_shared_value_clear_internal(&snapshot[i]);
	}
	free(snapshot);
	return array;
}

lt_Value ltshared_table_keys(lt_VM* vm, lt_SharedObject* shared)
{
	return _lt_shared_table_collect_values(vm, shared, 1);
}

lt_Value ltshared_table_values(lt_VM* vm, lt_SharedObject* shared)
{
	return _lt_shared_table_collect_values(vm, shared, 0);
}

lt_Value ltshared_array_get(lt_VM* vm, lt_SharedObject* shared, uint32_t idx)
{
	_lt_shared_lock(shared);
	if (idx >= shared->array.length)
	{
		_lt_shared_unlock(shared);
		return LT_VALUE_NULL;
	}
	lt_SharedValue found;
	if (!_lt_shared_value_clone(&found, &shared->array.values[idx]))
	{
		_lt_shared_unlock(shared);
		return LT_VALUE_NULL;
	}
	_lt_shared_unlock(shared);
	lt_Value value = lt_shared_to_vm_internal(vm, &found);
	lt_shared_value_clear_internal(&found);
	return value;
}

lt_Value ltshared_array_set(lt_VM* vm, lt_SharedObject* shared, uint32_t idx, lt_Value val)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_value;
	if (!lt_shared_from_vm_internal(&ctx, vm, val, &shared_value))
	{
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}

	_lt_shared_lock(shared);
	if (idx >= shared->array.length)
	{
		_lt_shared_unlock(shared);
		lt_shared_value_clear_internal(&shared_value);
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}
	lt_SharedValue old = shared->array.values[idx];
	shared->array.values[idx] = shared_value;
	_lt_shared_unlock(shared);
	lt_shared_value_clear_internal(&old);
	lt_shared_ctx_destroy_internal(&ctx);
	return val;
}

lt_Value ltshared_array_push(lt_VM* vm, lt_SharedObject* shared, lt_Value val)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_value;
	if (!lt_shared_from_vm_internal(&ctx, vm, val, &shared_value))
	{
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}

	_lt_shared_lock(shared);
	if (!lt_shared_reserve_internal((void**)&shared->array.values, &shared->array.capacity, shared->array.length, sizeof(lt_SharedValue)))
	{
		_lt_shared_unlock(shared);
		lt_shared_value_clear_internal(&shared_value);
		lt_shared_ctx_destroy_internal(&ctx);
		return LT_VALUE_NULL;
	}
	shared->array.values[shared->array.length++] = shared_value;
	_lt_shared_unlock(shared);
	lt_shared_ctx_destroy_internal(&ctx);
	return val;
}

lt_Value ltshared_array_remove(lt_VM* vm, lt_SharedObject* shared, uint32_t idx)
{
	_lt_shared_lock(shared);
	if (idx >= shared->array.length)
	{
		_lt_shared_unlock(shared);
		return LT_VALUE_NULL;
	}
	lt_SharedValue old_value = shared->array.values[idx];
	if (idx + 1 < shared->array.length)
		memmove(shared->array.values + idx, shared->array.values + idx + 1, (shared->array.length - idx - 1) * sizeof(lt_SharedValue));
	shared->array.length--;
	lt_shared_value_retain_external_internal(&old_value);
	_lt_shared_unlock(shared);
	lt_Value old = lt_shared_to_vm_internal(vm, &old_value);
	lt_shared_value_release_external_internal(&old_value);
	lt_shared_value_clear_internal(&old_value);
	return old;
}

uint32_t ltshared_array_length(lt_SharedObject* shared)
{
	_lt_shared_lock(shared);
	uint32_t length = shared->array.length;
	_lt_shared_unlock(shared);
	return length;
}
