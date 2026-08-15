#include "little_async.h"
#include "little_internal.h"
#include "little_std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#ifndef LT_TASK_SHARED_MAX_NODES
#define LT_TASK_SHARED_MAX_NODES 4096
#endif

#ifndef LT_TASK_SHARED_MAX_DEPTH
#define LT_TASK_SHARED_MAX_DEPTH 256
#endif

typedef enum {
	LT_REACTION_NEXT,
	LT_REACTION_CATCH,
	LT_REACTION_FINALLY,
} lt_ReactionKind;

typedef struct {
	lt_ReactionKind kind;
	lt_Value callback;
	lt_Value next_promise;
} lt_PromiseReaction;

typedef struct {
	lt_Value promise;
	lt_PromiseReaction reaction;
} lt_Microtask;

typedef struct {
	lt_Value promise;
	lt_Value callee;
	lt_Value args[LT_MAX_CALL_ARGS];
	uint8_t argc;
} lt_AsyncCall;

typedef struct {
	uint32_t id;
	uint64_t due_ms;
	uint64_t interval_ms;
	lt_Value callback;
	uint8_t repeat;
	uint8_t cancelled;
} lt_Timer;

typedef struct {
	uint32_t id;
	lt_PollHook hook;
	void* context;
	uint8_t removed;
} lt_PollHookEntry;

typedef enum {
	LT_SHARED_TABLE,
	LT_SHARED_ARRAY,
} lt_SharedKind;

typedef enum {
	LT_SHARED_VALUE_NULL,
	LT_SHARED_VALUE_BOOL,
	LT_SHARED_VALUE_NUMBER,
	LT_SHARED_VALUE_STRING,
	LT_SHARED_VALUE_OBJECT,
} lt_SharedValueType;

typedef struct {
	lt_SharedValueType type;
	union {
		uint8_t boolean;
		double number;
		char* string;
		lt_SharedObject* object;
	};
} lt_SharedValue;

typedef struct {
	lt_SharedValue key;
	lt_SharedValue value;
} lt_SharedPair;

struct lt_SharedObject {
	lt_SharedKind kind;
	uint32_t external_refs;
	uint8_t gc_mark;
	union {
		struct {
			lt_SharedPair* pairs;
			uint32_t length;
			uint32_t capacity;
		} table;
		struct {
			lt_SharedValue* values;
			uint32_t length;
			uint32_t capacity;
		} array;
	};
#if defined(_WIN32)
	CRITICAL_SECTION lock;
#else
	pthread_mutex_t lock;
#endif
	struct lt_SharedObject* next;
};

typedef struct {
	lt_Object* object;
	lt_SharedObject* shared;
} lt_SharedMarshalEntry;

typedef struct {
	lt_SharedMarshalEntry* entries;
	uint32_t length;
	uint32_t capacity;
	uint32_t depth;
	char error[160];
} lt_SharedMarshalCtx;

typedef struct {
	lt_VM* parent;
	lt_Object* promise;
	lt_VM* vm;
	lt_Value callable;
	lt_Value state_arg;
	lt_SharedValue result;
	uint8_t has_result;
	lt_Value state;
	lt_Value callee;
	char* error;
	uint8_t done;
#if defined(_WIN32)
	HANDLE handle;
	CRITICAL_SECTION lock;
#else
	pthread_t thread;
	pthread_mutex_t lock;
#endif
} lt_Worker;

static uint8_t _lt_promise_next(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_catch(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_finally(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_resolve_native(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_reject_native(lt_VM* vm, uint8_t argc);
static uint8_t _lt_worker_task_run_disabled(lt_VM* vm, uint8_t argc)
{
	lt_runtime_error(vm, "task.run is unavailable inside workers!");
	return 0;
}

static void _lt_worker_open_disabled_task(lt_VM* vm)
{
	lt_Value task = lt_make_table(vm);
	lt_table_set(vm, task, lt_make_string(vm, "run"), lt_make_native(vm, _lt_worker_task_run_disabled));
	lt_table_set(vm, vm->global, lt_make_string(vm, "task"), task);
}

static void _lt_worker_lock(lt_Worker* worker)
{
#if defined(_WIN32)
	EnterCriticalSection(&worker->lock);
#else
	pthread_mutex_lock(&worker->lock);
#endif
}

static void _lt_worker_unlock(lt_Worker* worker)
{
#if defined(_WIN32)
	LeaveCriticalSection(&worker->lock);
#else
	pthread_mutex_unlock(&worker->lock);
#endif
}

static void _lt_worker_init_lock(lt_Worker* worker)
{
#if defined(_WIN32)
	InitializeCriticalSection(&worker->lock);
#else
	pthread_mutex_init(&worker->lock, 0);
#endif
}

static void _lt_worker_destroy_lock(lt_Worker* worker)
{
#if defined(_WIN32)
	DeleteCriticalSection(&worker->lock);
#else
	pthread_mutex_destroy(&worker->lock);
#endif
}

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

static void _lt_shared_value_clear(lt_SharedValue* value)
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

static void _lt_shared_value_retain_external(lt_SharedValue* value)
{
	if (value->type == LT_SHARED_VALUE_OBJECT) ltshared_retain(value->object);
}

static void _lt_shared_value_release_external(lt_SharedValue* value)
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
			_lt_shared_value_clear(&shared->table.pairs[i].key);
			_lt_shared_value_clear(&shared->table.pairs[i].value);
		}
		free(shared->table.pairs);
	}
	else
	{
		for (uint32_t i = 0; i < shared->array.length; ++i)
			_lt_shared_value_clear(&shared->array.values[i]);
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

static uint8_t _lt_shared_reserve(void** data, uint32_t* capacity, uint32_t length, uint32_t element_size)
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
	if (!_lt_shared_reserve((void**)&ctx->entries, &ctx->capacity, ctx->length, sizeof(lt_SharedMarshalEntry))) return 0;
	ctx->entries[ctx->length++] = (lt_SharedMarshalEntry){ object, shared };
	return 1;
}

static void _lt_shared_ctx_destroy(lt_SharedMarshalCtx* ctx)
{
	free(ctx->entries);
	memset(ctx, 0, sizeof(lt_SharedMarshalCtx));
}

static uint8_t _lt_shared_from_vm(lt_SharedMarshalCtx* ctx, lt_VM* vm, lt_Value value, lt_SharedValue* out);

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
				if (!_lt_shared_reserve((void**)&shared->table.pairs, &shared->table.capacity, shared->table.length, sizeof(lt_SharedPair)))
				{
					snprintf(ctx->error, sizeof(ctx->error), "Failed to grow shared table!");
					return 0;
				}

				lt_SharedPair* next = &shared->table.pairs[shared->table.length];
				memset(next, 0, sizeof(lt_SharedPair));
				if (!_lt_shared_from_vm(ctx, vm, pair->key, &next->key)) return 0;
				if (!_lt_shared_from_vm(ctx, vm, pair->value, &next->value))
				{
					_lt_shared_value_clear(&next->key);
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
			if (!_lt_shared_reserve((void**)&shared->array.values, &shared->array.capacity, shared->array.length, sizeof(lt_SharedValue)))
			{
				snprintf(ctx->error, sizeof(ctx->error), "Failed to grow shared array!");
				return 0;
			}
			lt_SharedValue* next = &shared->array.values[shared->array.length];
			memset(next, 0, sizeof(lt_SharedValue));
			if (!_lt_shared_from_vm(ctx, vm, *(lt_Value*)lt_buffer_at(&object->array, i), next)) return 0;
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

static uint8_t _lt_shared_from_vm(lt_SharedMarshalCtx* ctx, lt_VM* vm, lt_Value value, lt_SharedValue* out)
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

static lt_Value _lt_shared_to_vm(lt_VM* vm, lt_SharedValue* value)
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
	if (!_lt_shared_from_vm(&ctx, vm, key, &shared_key))
	{
		_lt_shared_ctx_destroy(&ctx);
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
				_lt_shared_value_clear(&shared_key);
				_lt_shared_ctx_destroy(&ctx);
				return LT_VALUE_NULL;
			}
			_lt_shared_unlock(shared);
			lt_Value value = _lt_shared_to_vm(vm, &found);
			_lt_shared_value_clear(&found);
			_lt_shared_value_clear(&shared_key);
			_lt_shared_ctx_destroy(&ctx);
			return value;
		}
	}
	_lt_shared_unlock(shared);
	_lt_shared_value_clear(&shared_key);
	_lt_shared_ctx_destroy(&ctx);
	return LT_VALUE_NULL;
}

lt_Value ltshared_table_set(lt_VM* vm, lt_SharedObject* shared, lt_Value key, lt_Value val)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_key, shared_value;
	memset(&shared_key, 0, sizeof(shared_key));
	memset(&shared_value, 0, sizeof(shared_value));
	if (!_lt_shared_from_vm(&ctx, vm, key, &shared_key) || !_lt_shared_from_vm(&ctx, vm, val, &shared_value))
	{
		_lt_shared_value_clear(&shared_key);
		_lt_shared_value_clear(&shared_value);
		_lt_shared_ctx_destroy(&ctx);
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
			_lt_shared_value_clear(&old);
			_lt_shared_value_clear(&shared_key);
			_lt_shared_ctx_destroy(&ctx);
			return val;
		}
	}

	if (!_lt_shared_reserve((void**)&shared->table.pairs, &shared->table.capacity, shared->table.length, sizeof(lt_SharedPair)))
	{
		_lt_shared_unlock(shared);
		_lt_shared_value_clear(&shared_key);
		_lt_shared_value_clear(&shared_value);
		_lt_shared_ctx_destroy(&ctx);
		return LT_VALUE_NULL;
	}
	shared->table.pairs[shared->table.length++] = (lt_SharedPair){ shared_key, shared_value };
	_lt_shared_unlock(shared);
	_lt_shared_ctx_destroy(&ctx);
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
			_lt_shared_value_clear(&shared_key);
			_lt_shared_value_clear(&shared_value);
			_lt_shared_unlock(shared);
			return 0;
		}
		_lt_shared_value_retain_external(&shared_key);
		_lt_shared_value_retain_external(&shared_value);
		_lt_shared_unlock(shared);

		*key = _lt_shared_to_vm(vm, &shared_key);
		*val = _lt_shared_to_vm(vm, &shared_value);
		_lt_shared_value_release_external(&shared_key);
		_lt_shared_value_release_external(&shared_value);
		_lt_shared_value_clear(&shared_key);
		_lt_shared_value_clear(&shared_value);
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
		if (!_lt_shared_reserve((void**)&snapshot, &capacity, length, sizeof(lt_SharedValue)))
		{
			_lt_shared_unlock(shared);
			for (uint32_t j = 0; j < length; ++j)
			{
				_lt_shared_value_release_external(&snapshot[j]);
				_lt_shared_value_clear(&snapshot[j]);
			}
			free(snapshot);
			return lt_make_array(vm);
		}
		if (!_lt_shared_value_clone(&snapshot[length], keys ? &shared->table.pairs[i].key : &shared->table.pairs[i].value))
		{
			_lt_shared_unlock(shared);
			for (uint32_t j = 0; j < length; ++j)
			{
				_lt_shared_value_release_external(&snapshot[j]);
				_lt_shared_value_clear(&snapshot[j]);
			}
			free(snapshot);
			return lt_make_array(vm);
		}
		_lt_shared_value_retain_external(&snapshot[length]);
		length++;
	}
	_lt_shared_unlock(shared);

	lt_Value array = lt_make_array(vm);
	for (uint32_t i = 0; i < length; ++i)
	{
		lt_array_push(vm, array, _lt_shared_to_vm(vm, &snapshot[i]));
		_lt_shared_value_release_external(&snapshot[i]);
		_lt_shared_value_clear(&snapshot[i]);
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
	lt_Value value = _lt_shared_to_vm(vm, &found);
	_lt_shared_value_clear(&found);
	return value;
}

lt_Value ltshared_array_set(lt_VM* vm, lt_SharedObject* shared, uint32_t idx, lt_Value val)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_value;
	if (!_lt_shared_from_vm(&ctx, vm, val, &shared_value))
	{
		_lt_shared_ctx_destroy(&ctx);
		return LT_VALUE_NULL;
	}

	_lt_shared_lock(shared);
	if (idx >= shared->array.length)
	{
		_lt_shared_unlock(shared);
		_lt_shared_value_clear(&shared_value);
		_lt_shared_ctx_destroy(&ctx);
		return LT_VALUE_NULL;
	}
	lt_SharedValue old = shared->array.values[idx];
	shared->array.values[idx] = shared_value;
	_lt_shared_unlock(shared);
	_lt_shared_value_clear(&old);
	_lt_shared_ctx_destroy(&ctx);
	return val;
}

lt_Value ltshared_array_push(lt_VM* vm, lt_SharedObject* shared, lt_Value val)
{
	lt_SharedMarshalCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	lt_SharedValue shared_value;
	if (!_lt_shared_from_vm(&ctx, vm, val, &shared_value))
	{
		_lt_shared_ctx_destroy(&ctx);
		return LT_VALUE_NULL;
	}

	_lt_shared_lock(shared);
	if (!_lt_shared_reserve((void**)&shared->array.values, &shared->array.capacity, shared->array.length, sizeof(lt_SharedValue)))
	{
		_lt_shared_unlock(shared);
		_lt_shared_value_clear(&shared_value);
		_lt_shared_ctx_destroy(&ctx);
		return LT_VALUE_NULL;
	}
	shared->array.values[shared->array.length++] = shared_value;
	_lt_shared_unlock(shared);
	_lt_shared_ctx_destroy(&ctx);
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
	_lt_shared_value_retain_external(&old_value);
	_lt_shared_unlock(shared);
	lt_Value old = _lt_shared_to_vm(vm, &old_value);
	_lt_shared_value_release_external(&old_value);
	_lt_shared_value_clear(&old_value);
	return old;
}

uint32_t ltshared_array_length(lt_SharedObject* shared)
{
	_lt_shared_lock(shared);
	uint32_t length = shared->array.length;
	_lt_shared_unlock(shared);
	return length;
}

static uint8_t _lt_worker_is_done(lt_Worker* worker)
{
	_lt_worker_lock(worker);
	uint8_t done = worker->done;
	_lt_worker_unlock(worker);
	return done;
}

static void _lt_worker_mark_done(lt_Worker* worker)
{
	_lt_worker_lock(worker);
	worker->done = 1;
	_lt_worker_unlock(worker);
}

static void _lt_worker_set_error(lt_Worker* worker, const char* msg)
{
	_lt_worker_lock(worker);
	if (!worker->error)
	{
		uint32_t len = (uint32_t)strlen(msg);
		worker->error = malloc(len + 1);
		if (worker->error) memcpy(worker->error, msg, len + 1);
	}
	_lt_worker_unlock(worker);
}

static uint64_t _lt_now_ms(void)
{
#if defined(_WIN32)
	LARGE_INTEGER freq;
	LARGE_INTEGER counter;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	return (uint64_t)((counter.QuadPart * 1000ULL) / freq.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static void _lt_sleep_ms(uint32_t ms)
{
#if defined(_WIN32)
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

static lt_Value _lt_make_promise(lt_VM* vm)
{
	lt_Object* promise = lt_allocate(vm, LT_OBJECT_PROMISE);
	promise->promise.state = LT_PROMISE_PENDING;
	promise->promise.result = LT_VALUE_NULL;
	promise->promise.reactions = lt_buffer_new(sizeof(lt_PromiseReaction));
	promise->promise.handled = 0;
	return LT_VALUE_OBJECT(promise);
}

static lt_Value _lt_make_bound_native(lt_VM* vm, lt_NativeFn fn, lt_Value receiver)
{
	lt_Object* bound = lt_allocate(vm, LT_OBJECT_BOUND_NATIVE);
	bound->bound_native.native = fn;
	bound->bound_native.receiver = receiver;
	return LT_VALUE_OBJECT(bound);
}

static lt_Value _lt_current_receiver(lt_VM* vm)
{
	if (!vm->current || !vm->current->callee || vm->current->callee->type != LT_OBJECT_BOUND_NATIVE)
		return LT_VALUE_NULL;
	return vm->current->callee->bound_native.receiver;
}

static uint8_t _lt_is_callable(lt_Value value)
{
	return LT_IS_FUNCTION(value) || LT_IS_CLOSURE(value) || LT_IS_NATIVE(value) ||
		(LT_IS_OBJECT(value) && LT_GET_OBJECT(value)->type == LT_OBJECT_BOUND_NATIVE);
}

static void _lt_keep_value(lt_VM* vm, lt_Value value);
static void _lt_release_value(lt_VM* vm, lt_Value value);

static void _lt_schedule_reaction(lt_VM* vm, lt_Value promise, lt_PromiseReaction reaction)
{
	lt_Microtask task;
	task.promise = promise;
	task.reaction = reaction;
	lt_buffer_push(vm, &vm->microtasks, &task);
}

static void _lt_settle_promise(lt_VM* vm, lt_Value promise_value, lt_PromiseState state, lt_Value result)
{
	if (!LT_IS_OBJECT(promise_value)) return;
	lt_Object* promise = LT_GET_OBJECT(promise_value);
	if (promise->type != LT_OBJECT_PROMISE || promise->promise.state != LT_PROMISE_PENDING) return;

	promise->promise.state = state;
	promise->promise.result = result;
	for (uint32_t i = 0; i < promise->promise.reactions.length; ++i)
	{
		lt_PromiseReaction reaction = *(lt_PromiseReaction*)lt_buffer_at(&promise->promise.reactions, i);
		_lt_schedule_reaction(vm, promise_value, reaction);
	}
	promise->promise.reactions.length = 0;
}

static void _lt_add_promise_reaction(lt_VM* vm, lt_Value promise_value, lt_PromiseReaction reaction)
{
	lt_Object* promise = LT_GET_OBJECT(promise_value);
	_lt_keep_value(vm, promise_value);
	_lt_keep_value(vm, reaction.callback);
	_lt_keep_value(vm, reaction.next_promise);
	if (promise->promise.state == LT_PROMISE_PENDING)
		lt_buffer_push(vm, &promise->promise.reactions, &reaction);
	else
		_lt_schedule_reaction(vm, promise_value, reaction);
	_lt_release_value(vm, reaction.next_promise);
	_lt_release_value(vm, reaction.callback);
	_lt_release_value(vm, promise_value);
}

static void _lt_keep_value(lt_VM* vm, lt_Value value)
{
	if (LT_IS_OBJECT(value)) lt_nocollect(vm, LT_GET_OBJECT(value));
}

static void _lt_release_value(lt_VM* vm, lt_Value value)
{
	if (LT_IS_OBJECT(value)) lt_resumecollect(vm, LT_GET_OBJECT(value));
}

void ltasync_init_state(lt_VM* vm)
{
	vm->microtasks = lt_buffer_new(sizeof(lt_Microtask));
	vm->async_calls = lt_buffer_new(sizeof(lt_AsyncCall));
	vm->timers = lt_buffer_new(sizeof(lt_Timer));
	vm->workers = lt_buffer_new(sizeof(lt_Worker*));
	vm->poll_hooks = lt_buffer_new(sizeof(lt_PollHookEntry));
	vm->next_timer_id = 1;
	vm->next_poll_hook_id = 1;
	vm->runloop_stop = 0;
}

static void _lt_worker_destroy(lt_VM* vm, lt_Worker* worker, uint8_t join)
{
	if (join)
	{
#if defined(_WIN32)
		WaitForSingleObject(worker->handle, INFINITE);
		CloseHandle(worker->handle);
#else
		pthread_join(worker->thread, 0);
#endif
	}
	_lt_worker_destroy_lock(worker);
	if (worker->vm) lt_destroy(worker->vm);
	if (worker->error) free(worker->error);
	if (worker->has_result)
	{
		_lt_shared_value_release_external(&worker->result);
		_lt_shared_value_clear(&worker->result);
	}
	vm->free(worker);
}

void ltasync_destroy_state(lt_VM* vm)
{
	for (uint32_t i = 0; i < vm->workers.length; ++i)
	{
		lt_Worker* worker = *(lt_Worker**)lt_buffer_at(&vm->workers, i);
		_lt_worker_destroy(vm, worker, 1);
	}
	lt_buffer_destroy(vm, &vm->microtasks);
	lt_buffer_destroy(vm, &vm->async_calls);
	lt_buffer_destroy(vm, &vm->timers);
	lt_buffer_destroy(vm, &vm->workers);
	lt_buffer_destroy(vm, &vm->poll_hooks);
}

uint32_t lt_add_poll_hook(lt_VM* vm, lt_PollHook hook, void* context)
{
	if (!hook) return 0;
	uint32_t id = vm->next_poll_hook_id++;
	if (id == 0) id = vm->next_poll_hook_id++;
	lt_PollHookEntry entry = { id, hook, context, 0 };
	lt_buffer_push(vm, &vm->poll_hooks, &entry);
	return id;
}

void lt_remove_poll_hook(lt_VM* vm, uint32_t hook_id)
{
	if (hook_id == 0) return;
	for (uint32_t i = 0; i < vm->poll_hooks.length; ++i)
	{
		lt_PollHookEntry* entry = lt_buffer_at(&vm->poll_hooks, i);
		if (entry->id == hook_id)
		{
			entry->removed = 1;
			return;
		}
	}
}

uint8_t ltasync_poll_hooks(lt_VM* vm, uint8_t* pending)
{
	uint8_t did_work = 0;
	/* Hooks added by a callback begin on the next poll. This also prevents a
	   callback which registers another hook from extending this dispatch. */
	uint32_t hook_count = vm->poll_hooks.length;
	for (uint32_t i = 0; i < hook_count; ++i)
	{
		lt_PollHookEntry entry = *(lt_PollHookEntry*)lt_buffer_at(&vm->poll_hooks, i);
		if (entry.removed) continue;
		lt_PollResult result = entry.hook(vm, entry.context);
		if (result == LT_POLL_WORK) did_work = 1;
		if (result == LT_POLL_WORK || result == LT_POLL_PENDING) *pending = 1;
	}

	for (uint32_t i = 0; i < vm->poll_hooks.length; ++i)
	{
		lt_PollHookEntry* entry = lt_buffer_at(&vm->poll_hooks, i);
		if (entry->removed) lt_buffer_cycle(&vm->poll_hooks, i--);
	}
	return did_work;
}

void ltasync_free_promise(lt_VM* vm, lt_Object* promise)
{
	lt_buffer_destroy(vm, &promise->promise.reactions);
}

void ltasync_mark_promise(lt_VM* vm, lt_Object* promise)
{
	lt_sweep_v(vm, promise->promise.result);
	for (uint32_t i = 0; i < promise->promise.reactions.length; ++i)
	{
		lt_PromiseReaction* reaction = lt_buffer_at(&promise->promise.reactions, i);
		lt_sweep_v(vm, reaction->callback);
		lt_sweep_v(vm, reaction->next_promise);
	}
}

void ltasync_mark_roots(lt_VM* vm)
{
	for (uint32_t i = 0; i < vm->timers.length; ++i)
	{
		lt_Timer* timer = lt_buffer_at(&vm->timers, i);
		if (!timer->cancelled) lt_sweep_v(vm, timer->callback);
	}

	for (uint32_t i = 0; i < vm->microtasks.length; ++i)
	{
		lt_Microtask* task = lt_buffer_at(&vm->microtasks, i);
		lt_sweep_v(vm, task->promise);
		lt_sweep_v(vm, task->reaction.callback);
		lt_sweep_v(vm, task->reaction.next_promise);
	}

	for (uint32_t i = 0; i < vm->async_calls.length; ++i)
	{
		lt_AsyncCall* task = lt_buffer_at(&vm->async_calls, i);
		lt_sweep_v(vm, task->promise);
		lt_sweep_v(vm, task->callee);
		for (uint8_t j = 0; j < task->argc; ++j) lt_sweep_v(vm, task->args[j]);
	}

	for (uint32_t i = 0; i < vm->workers.length; ++i)
	{
		lt_Worker* worker = *(lt_Worker**)lt_buffer_at(&vm->workers, i);
		lt_sweep_v(vm, LT_VALUE_OBJECT(worker->promise));
		lt_sweep_v(vm, worker->state);
		lt_sweep_v(vm, worker->callee);
	}
}

uint8_t lt_poll(lt_VM* vm)
{
	uint8_t did_work = 0;
	uint8_t has_pending = 0;
	uint8_t has_next_timer = 0;
	uint64_t next_timer_due = 0;
	did_work = ltasync_poll_hooks(vm, &has_pending);

	if (vm->async_calls.length > 0)
	{
		lt_AsyncCall task = *(lt_AsyncCall*)lt_buffer_at(&vm->async_calls, 0);
		lt_buffer_cycle(&vm->async_calls, 0);
		_lt_keep_value(vm, task.promise);
		_lt_keep_value(vm, task.callee);
		for (uint8_t i = 0; i < task.argc; ++i) _lt_keep_value(vm, task.args[i]);

		for (uint8_t i = 0; i < task.argc; ++i) lt_push(vm, task.args[i]);
		uint16_t nret = lt_exec(vm, task.callee, task.argc);
		lt_Value result = LT_VALUE_NULL;
		if (nret > 0)
		{
			result = lt_pop(vm);
			for (uint16_t i = 1; i < nret; ++i) lt_pop(vm);
		}

		_lt_settle_promise(vm, task.promise, LT_PROMISE_FULFILLED, result);
		for (uint8_t i = task.argc; i > 0; --i) _lt_release_value(vm, task.args[i - 1]);
		_lt_release_value(vm, task.callee);
		_lt_release_value(vm, task.promise);
		return 1;
	}

	if (vm->microtasks.length > 0)
	{
		lt_Microtask task = *(lt_Microtask*)lt_buffer_at(&vm->microtasks, 0);
		lt_buffer_cycle(&vm->microtasks, 0);

		lt_Object* source = LT_GET_OBJECT(task.promise);
		lt_Value result = source->promise.result;
		lt_PromiseState state = source->promise.state;
		uint8_t should_call = 0;
		uint8_t propagate_original = 0;
		_lt_keep_value(vm, task.promise);
		_lt_keep_value(vm, result);
		_lt_keep_value(vm, task.reaction.callback);
		_lt_keep_value(vm, task.reaction.next_promise);

		if (task.reaction.kind == LT_REACTION_FINALLY)
		{
			should_call = 1;
			propagate_original = 1;
		}
		else if (state == LT_PROMISE_FULFILLED && task.reaction.kind == LT_REACTION_NEXT)
		{
			should_call = 1;
		}
		else if (state == LT_PROMISE_REJECTED && task.reaction.kind == LT_REACTION_CATCH)
		{
			should_call = 1;
		}

		if (should_call)
		{
			uint16_t nret = 0;
			if (task.reaction.kind == LT_REACTION_FINALLY)
			{
				nret = lt_exec(vm, task.reaction.callback, 0);
			}
			else
			{
				lt_push(vm, result);
				nret = lt_exec(vm, task.reaction.callback, 1);
			}

			lt_Value callback_result = LT_VALUE_NULL;
			if (nret > 0)
			{
				callback_result = lt_pop(vm);
				for (uint16_t i = 1; i < nret; ++i) lt_pop(vm);
			}

			if (propagate_original) _lt_settle_promise(vm, task.reaction.next_promise, state, result);
			else _lt_settle_promise(vm, task.reaction.next_promise, LT_PROMISE_FULFILLED, callback_result);
		}
		else
		{
			_lt_settle_promise(vm, task.reaction.next_promise, state, result);
		}

		_lt_release_value(vm, task.reaction.next_promise);
		_lt_release_value(vm, task.reaction.callback);
		_lt_release_value(vm, result);
		_lt_release_value(vm, task.promise);
		return 1;
	}

	for (uint32_t i = 0; i < vm->workers.length; ++i)
	{
		lt_Worker* worker = *(lt_Worker**)lt_buffer_at(&vm->workers, i);
		if (!_lt_worker_is_done(worker))
		{
			has_pending = 1;
			continue;
		}

		lt_Value promise = LT_VALUE_OBJECT(worker->promise);
		if (worker->error)
			_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, worker->error));
		else
			_lt_settle_promise(vm, promise, LT_PROMISE_FULFILLED, worker->has_result ? _lt_shared_to_vm(vm, &worker->result) : LT_VALUE_NULL);

		_lt_worker_destroy(vm, worker, 1);
		lt_buffer_cycle(&vm->workers, i--);
		return 1;
	}

	uint64_t now = _lt_now_ms();

	for (uint32_t i = 0; i < vm->timers.length; ++i)
	{
		lt_Timer* timer = lt_buffer_at(&vm->timers, i);
		if (timer->cancelled)
		{
			lt_buffer_cycle(&vm->timers, i--);
			continue;
		}

		if (timer->due_ms <= now)
		{
			lt_Timer fired = *timer;
			uint16_t nret = lt_exec(vm, fired.callback, 0);
			while (nret-- > 0) lt_pop(vm);

			uint32_t current_idx = UINT32_MAX;
			for (uint32_t j = 0; j < vm->timers.length; ++j)
			{
				lt_Timer* current = lt_buffer_at(&vm->timers, j);
				if (current->id == fired.id)
				{
					current_idx = j;
					break;
				}
			}

			if (current_idx != UINT32_MAX)
			{
				lt_Timer* current = lt_buffer_at(&vm->timers, current_idx);
				if (current->repeat && !current->cancelled)
				{
					current->due_ms = _lt_now_ms() + current->interval_ms;
					has_pending = 1;
				}
				else
				{
					lt_buffer_cycle(&vm->timers, current_idx);
				}
			}

			return 1;
		}
		else
		{
			has_pending = 1;
			if (!has_next_timer || timer->due_ms < next_timer_due)
			{
				has_next_timer = 1;
				next_timer_due = timer->due_ms;
			}
		}
	}

	if (!did_work && (has_pending || vm->workers.length > 0))
	{
		uint32_t sleep_ms = 1;
		if (has_next_timer && next_timer_due > now)
		{
			uint64_t until_due = next_timer_due - now;
			if (vm->workers.length > 0 && until_due > 10) until_due = 10;
			if (until_due > UINT32_MAX) until_due = UINT32_MAX;
			sleep_ms = (uint32_t)until_due;
		}
		if (sleep_ms == 0) sleep_ms = 1;
		_lt_sleep_ms(sleep_ms);
	}
	return did_work || has_pending || vm->async_calls.length > 0 || vm->microtasks.length > 0 || vm->workers.length > 0;
}

void lt_runloop(lt_VM* vm)
{
	while (!vm->runloop_stop && lt_poll(vm)) {}
}

static uint8_t ltasync_native_mainloop_run(lt_VM* vm, uint8_t argc)
{
	if (argc != 0) lt_runtime_error(vm, "Expected no arguments to mainloop.run!");
	vm->runloop_stop = 0;
	uint32_t count = 0;
	while (!vm->runloop_stop && lt_poll(vm)) count++;
	lt_push(vm, lt_make_number((double)count));
	return 1;
}

static uint8_t ltasync_native_mainloop_poll(lt_VM* vm, uint8_t argc)
{
	if (argc != 0) lt_runtime_error(vm, "Expected no arguments to mainloop.poll!");
	lt_push(vm, lt_poll(vm) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
	return 1;
}

static uint8_t ltasync_native_mainloop_stop(lt_VM* vm, uint8_t argc)
{
	if (argc != 0) lt_runtime_error(vm, "Expected no arguments to mainloop.stop!");
	vm->runloop_stop = 1;
	return 0;
}

static uint8_t _lt_add_timer(lt_VM* vm, uint8_t argc, uint8_t repeat)
{
	if (argc != 2) lt_runtime_error(vm, repeat ? "Expected callback and delay for setInterval!" : "Expected callback and delay for setTimeout!");
	lt_Value delay = lt_pop(vm);
	lt_Value callback = lt_pop(vm);
	if (!LT_IS_NUMBER(delay)) lt_runtime_error(vm, "Expected timer delay to be a number!");
	if (!_lt_is_callable(callback)) lt_runtime_error(vm, "Expected timer callback to be callable!");

	double delay_number = lt_get_number(delay);
	if (delay_number < 0) lt_runtime_error(vm, "Expected timer delay to be non-negative!");
	uint64_t delay_ms = (uint64_t)delay_number;
	lt_Timer timer;
	timer.id = vm->next_timer_id++;
	timer.due_ms = _lt_now_ms() + delay_ms;
	timer.interval_ms = delay_ms;
	timer.callback = callback;
	timer.repeat = repeat;
	timer.cancelled = 0;
	lt_buffer_push(vm, &vm->timers, &timer);

	lt_push(vm, lt_make_number((double)timer.id));
	return 1;
}

uint8_t ltasync_native_set_timeout(lt_VM* vm, uint8_t argc)
{
	return _lt_add_timer(vm, argc, 0);
}

uint8_t ltasync_native_set_interval(lt_VM* vm, uint8_t argc)
{
	return _lt_add_timer(vm, argc, 1);
}

uint8_t ltasync_native_clear_timer(lt_VM* vm, uint8_t argc)
{
	if (argc != 1) lt_runtime_error(vm, "Expected timer id to clear!");
	lt_Value id_val = lt_pop(vm);
	if (!LT_IS_NUMBER(id_val)) lt_runtime_error(vm, "Expected timer id to be a number!");
	uint32_t id = (uint32_t)lt_get_number(id_val);

	for (uint32_t i = 0; i < vm->timers.length; ++i)
	{
		lt_Timer* timer = lt_buffer_at(&vm->timers, i);
		if (timer->id == id)
		{
			timer->cancelled = 1;
			break;
		}
	}

	return 0;
}

uint8_t ltasync_native_promise(lt_VM* vm, uint8_t argc)
{
	if (argc != 1) lt_runtime_error(vm, "Expected executor function for Promise!");
	lt_Value executor = lt_pop(vm);
	if (!_lt_is_callable(executor)) lt_runtime_error(vm, "Expected Promise executor to be callable!");

	lt_Value promise = _lt_make_promise(vm);
	lt_nocollect(vm, LT_GET_OBJECT(promise));
	lt_Value resolve = _lt_make_bound_native(vm, _lt_promise_resolve_native, promise);
	lt_Value reject = _lt_make_bound_native(vm, _lt_promise_reject_native, promise);

	lt_push(vm, resolve);
	lt_push(vm, reject);
	uint16_t nret = lt_exec(vm, executor, 2);
	while (nret-- > 0) lt_pop(vm);

	lt_push(vm, promise);
	lt_resumecollect(vm, LT_GET_OBJECT(promise));
	return 1;
}

static uint8_t _lt_promise_resolve_native(lt_VM* vm, uint8_t argc)
{
	lt_Value promise = _lt_current_receiver(vm);
	lt_Value value = argc > 0 ? lt_pop(vm) : LT_VALUE_NULL;
	while (argc-- > 1) lt_pop(vm);
	_lt_settle_promise(vm, promise, LT_PROMISE_FULFILLED, value);
	return 0;
}

static uint8_t _lt_promise_reject_native(lt_VM* vm, uint8_t argc)
{
	lt_Value promise = _lt_current_receiver(vm);
	lt_Value value = argc > 0 ? lt_pop(vm) : LT_VALUE_NULL;
	while (argc-- > 1) lt_pop(vm);
	_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, value);
	return 0;
}

static uint8_t _lt_add_promise_callback(lt_VM* vm, uint8_t argc, lt_ReactionKind kind)
{
	if (argc != 1) lt_runtime_error(vm, "Expected one promise callback!");
	lt_Value callback = lt_pop(vm);
	if (!_lt_is_callable(callback)) lt_runtime_error(vm, "Expected promise callback to be callable!");

	lt_Value promise = _lt_current_receiver(vm);
	if (!LT_IS_OBJECT(promise) || LT_GET_OBJECT(promise)->type != LT_OBJECT_PROMISE)
		lt_runtime_error(vm, "Invalid promise receiver!");

	lt_PromiseReaction reaction;
	reaction.kind = kind;
	reaction.callback = callback;
	reaction.next_promise = _lt_make_promise(vm);
	_lt_add_promise_reaction(vm, promise, reaction);

	lt_push(vm, reaction.next_promise);
	return 1;
}

static uint8_t _lt_promise_next(lt_VM* vm, uint8_t argc)
{
	return _lt_add_promise_callback(vm, argc, LT_REACTION_NEXT);
}

static uint8_t _lt_promise_catch(lt_VM* vm, uint8_t argc)
{
	return _lt_add_promise_callback(vm, argc, LT_REACTION_CATCH);
}

static uint8_t _lt_promise_finally(lt_VM* vm, uint8_t argc)
{
	return _lt_add_promise_callback(vm, argc, LT_REACTION_FINALLY);
}

lt_Value ltasync_get_promise_method(lt_VM* vm, lt_Value promise, lt_Value key)
{
	const char* method = lt_get_string(vm, key);
	if (strcmp(method, "next") == 0) return _lt_make_bound_native(vm, _lt_promise_next, promise);
	if (strcmp(method, "catch") == 0) return _lt_make_bound_native(vm, _lt_promise_catch, promise);
	if (strcmp(method, "finally") == 0) return _lt_make_bound_native(vm, _lt_promise_finally, promise);
	return LT_VALUE_NULL;
}

uint8_t ltasync_is_async_callable(lt_Value callable)
{
	if (!LT_IS_OBJECT(callable)) return 0;
	lt_Object* callee = LT_GET_OBJECT(callable);
	if (callee->type == LT_OBJECT_FN) return callee->fn.is_async;
	if (callee->type == LT_OBJECT_CLOSURE)
	{
		lt_Object* fn = LT_GET_OBJECT(callee->closure.function);
		return fn->type == LT_OBJECT_FN && fn->fn.is_async;
	}
	return 0;
}

lt_Value ltasync_call(lt_VM* vm, lt_Value callee, uint8_t argc)
{
	if (argc > LT_MAX_CALL_ARGS) lt_runtime_error(vm, "Too many async call arguments!");

	lt_Value promise = _lt_make_promise(vm);
	lt_AsyncCall task;
	memset(&task, 0, sizeof(task));
	task.promise = promise;
	task.callee = callee;
	task.argc = argc;

	for (uint8_t i = 0; i < argc; ++i)
		task.args[i] = vm->stack[vm->top - argc + i];

	lt_push(vm, promise);
	lt_push(vm, callee);
	lt_buffer_push(vm, &vm->async_calls, &task);
	lt_pop(vm);
	lt_pop(vm);
	vm->top -= argc;
	return promise;
}

lt_Value ltasync_await(lt_VM* vm, lt_Value value)
{
	if (!LT_IS_OBJECT(value) || LT_GET_OBJECT(value)->type != LT_OBJECT_PROMISE) return value;

	lt_Object* promise = LT_GET_OBJECT(value);
	lt_nocollect(vm, promise);
	while (promise->promise.state == LT_PROMISE_PENDING)
	{
		if (!lt_poll(vm)) break;
	}

	if (promise->promise.state == LT_PROMISE_PENDING)
	{
		lt_resumecollect(vm, promise);
		lt_runtime_error(vm, "Awaited promise was not resolved!");
	}

	if (promise->promise.state == LT_PROMISE_REJECTED)
	{
		lt_resumecollect(vm, promise);
		if (LT_IS_STRING(promise->promise.result)) lt_runtime_error(vm, lt_get_string(vm, promise->promise.result));
		lt_runtime_error(vm, "Awaited promise rejected!");
	}

	lt_Value result = promise->promise.result;
	lt_resumecollect(vm, promise);
	return result;
}

static void _lt_worker_error(lt_VM* vm, const char* msg)
{
	lt_Worker* worker = vm->error_context;
	if (!worker) return;

	_lt_worker_set_error(worker, msg);
}

typedef struct {
	lt_Object* source;
	lt_Object* imported;
} lt_ImportEntry;

typedef struct {
	lt_ImportEntry* entries;
	uint32_t length;
	uint32_t capacity;
	char error[160];
} lt_ImportCtx;

static void _lt_import_ctx_destroy(lt_ImportCtx* ctx)
{
	free(ctx->entries);
	memset(ctx, 0, sizeof(lt_ImportCtx));
}

static lt_Object* _lt_import_ctx_find(lt_ImportCtx* ctx, lt_Object* source)
{
	for (uint32_t i = 0; i < ctx->length; ++i)
		if (ctx->entries[i].source == source) return ctx->entries[i].imported;
	return 0;
}

static uint8_t _lt_import_ctx_add(lt_ImportCtx* ctx, lt_Object* source, lt_Object* imported)
{
	if (!_lt_shared_reserve((void**)&ctx->entries, &ctx->capacity, ctx->length, sizeof(lt_ImportEntry))) return 0;
	ctx->entries[ctx->length++] = (lt_ImportEntry){ source, imported };
	return 1;
}

static uint8_t _lt_import_buffer(lt_VM* worker_vm, lt_Buffer* dst, lt_Buffer* src)
{
	*dst = lt_buffer_new(src->element_size);
	for (uint32_t i = 0; i < src->length; ++i)
		lt_buffer_push(worker_vm, dst, lt_buffer_at(src, i));
	return 1;
}

static uint8_t _lt_import_value(lt_ImportCtx* ctx, lt_VM* parent_vm, lt_VM* worker_vm, lt_Value value, lt_Value* out);

static lt_Object* _lt_import_fn(lt_ImportCtx* ctx, lt_VM* parent_vm, lt_VM* worker_vm, lt_Object* source)
{
	lt_Object* existing = _lt_import_ctx_find(ctx, source);
	if (existing) return existing;

	lt_Object* fn = lt_allocate(worker_vm, LT_OBJECT_FN);
	fn->fn.arity = source->fn.arity;
	fn->fn.is_async = source->fn.is_async;
	fn->fn.owner_class = 0;
	fn->fn.debug = 0;
	_lt_import_buffer(worker_vm, &fn->fn.code, &source->fn.code);
	fn->fn.constants = lt_buffer_new(sizeof(lt_Value));

	if (!_lt_import_ctx_add(ctx, source, fn))
	{
		snprintf(ctx->error, sizeof(ctx->error), "Failed to track imported task callable!");
		return 0;
	}

	for (uint32_t i = 0; i < source->fn.constants.length; ++i)
	{
		lt_Value imported = LT_VALUE_NULL;
		if (!_lt_import_value(ctx, parent_vm, worker_vm, *(lt_Value*)lt_buffer_at(&source->fn.constants, i), &imported))
			return 0;
		lt_buffer_push(worker_vm, &fn->fn.constants, &imported);
	}

	return fn;
}

static uint8_t _lt_import_value(lt_ImportCtx* ctx, lt_VM* parent_vm, lt_VM* worker_vm, lt_Value value, lt_Value* out)
{
	if (LT_IS_NULL(value) || LT_IS_BOOL(value) || LT_IS_NUMBER(value))
	{
		*out = value;
		return 1;
	}
	if (LT_IS_STRING(value))
	{
		*out = lt_make_string(worker_vm, lt_get_string(parent_vm, value));
		return 1;
	}
	if (LT_IS_OBJECT(value))
	{
		lt_Object* object = LT_GET_OBJECT(value);
		if (object->type == LT_OBJECT_FN)
		{
			lt_Object* imported = _lt_import_fn(ctx, parent_vm, worker_vm, object);
			if (!imported) return 0;
			*out = LT_VALUE_OBJECT(imported);
			return 1;
		}
	}

	snprintf(ctx->error, sizeof(ctx->error), "Unsupported task callable constant!");
	return 0;
}

static uint8_t _lt_import_callable(lt_ImportCtx* ctx, lt_VM* parent_vm, lt_VM* worker_vm, lt_Value callable, lt_Value* out)
{
	if (!LT_IS_OBJECT(callable))
	{
		snprintf(ctx->error, sizeof(ctx->error), "Expected task.run callable to be a function!");
		return 0;
	}

	lt_Object* object = LT_GET_OBJECT(callable);
	if (object->type == LT_OBJECT_CLOSURE)
	{
		if (object->closure.captures.length > 0)
		{
			snprintf(ctx->error, sizeof(ctx->error), "task.run closures cannot capture values; pass explicit state instead!");
			return 0;
		}
		object = LT_GET_OBJECT(object->closure.function);
	}
	if (object->type != LT_OBJECT_FN)
	{
		snprintf(ctx->error, sizeof(ctx->error), "Expected task.run callable to be a Little function!");
		return 0;
	}

	lt_Object* imported_fn = _lt_import_fn(ctx, parent_vm, worker_vm, object);
	if (!imported_fn) return 0;
	*out = LT_VALUE_OBJECT(imported_fn);
	return 1;
}

#if defined(_WIN32)
static DWORD WINAPI _lt_worker_main(LPVOID arg)
#else
static void* _lt_worker_main(void* arg)
#endif
{
	lt_Worker* worker = (lt_Worker*)arg;

	lt_VM* vm = worker->vm;
	_lt_keep_value(vm, worker->callable);
	lt_push(vm, worker->state_arg);
	_lt_release_value(vm, worker->state_arg);
	_lt_release_value(vm, worker->callable);
	uint32_t nret = lt_exec(vm, worker->callable, 1);
	_lt_release_value(vm, worker->callable);

	_lt_worker_lock(worker);
	uint8_t has_error = worker->error != 0;
	_lt_worker_unlock(worker);

	if (!has_error)
	{
		lt_Value value = nret > 0 ? lt_pop(vm) : LT_VALUE_NULL;
		if (LT_IS_OBJECT(value) && LT_GET_OBJECT(value)->type == LT_OBJECT_PROMISE)
		{
			lt_Object* promise = LT_GET_OBJECT(value);
			lt_nocollect(vm, promise);
			while (promise->promise.state == LT_PROMISE_PENDING)
			{
				if (!lt_poll(vm)) break;
			}
			if (promise->promise.state == LT_PROMISE_PENDING)
			{
				_lt_worker_set_error(worker, "Task promise was not resolved!");
			}
			else if (promise->promise.state == LT_PROMISE_REJECTED)
			{
				if (LT_IS_STRING(promise->promise.result)) _lt_worker_set_error(worker, lt_get_string(vm, promise->promise.result));
				else _lt_worker_set_error(worker, "Task promise rejected!");
			}
			else value = promise->promise.result;
			lt_resumecollect(vm, promise);
		}

		_lt_worker_lock(worker);
		has_error = worker->error != 0;
		_lt_worker_unlock(worker);

		if (!has_error)
		{
			lt_SharedMarshalCtx ctx;
			memset(&ctx, 0, sizeof(ctx));
			if (_lt_shared_from_vm(&ctx, vm, value, &worker->result))
			{
				_lt_shared_value_retain_external(&worker->result);
				_lt_worker_lock(worker);
				worker->has_result = 1;
				_lt_worker_unlock(worker);
			}
			else _lt_worker_set_error(worker, ctx.error[0] ? ctx.error : "Unsupported task result crossing worker boundary!");
			_lt_shared_ctx_destroy(&ctx);
		}
	}

	lt_destroy(vm);
	worker->vm = 0;
	_lt_worker_mark_done(worker);
#if defined(_WIN32)
	return 0;
#else
	return 0;
#endif
}

uint8_t ltasync_native_task_run(lt_VM* vm, uint8_t argc)
{
	if (argc != 2) lt_runtime_error(vm, "Expected callable and state for task.run!");
	lt_Value state = lt_pop(vm);
	lt_Value callable = lt_pop(vm);
	if (!_lt_is_callable(callable) || LT_IS_NATIVE(callable) ||
		(LT_IS_OBJECT(callable) && LT_GET_OBJECT(callable)->type == LT_OBJECT_BOUND_NATIVE))
		lt_runtime_error(vm, "Expected task.run callable to be a Little function!");

	lt_Value promise = _lt_make_promise(vm);
	lt_nocollect(vm, LT_GET_OBJECT(promise));
	_lt_keep_value(vm, state);
	_lt_keep_value(vm, callable);

	lt_Worker* worker = vm->alloc(sizeof(lt_Worker));
	if (!worker)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to allocate worker!"));
		lt_push(vm, promise);
		_lt_release_value(vm, callable);
		_lt_release_value(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	memset(worker, 0, sizeof(lt_Worker));
	_lt_worker_init_lock(worker);
	worker->parent = vm;
	worker->promise = LT_GET_OBJECT(promise);
	worker->state = state;
	worker->callee = callable;

	worker->vm = lt_open(malloc, free, _lt_worker_error);
	if (!worker->vm)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker VM!"));
		_lt_worker_destroy_lock(worker);
		vm->free(worker);
		lt_push(vm, promise);
		_lt_release_value(vm, callable);
		_lt_release_value(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	worker->vm->error_context = worker;
	ltstd_open_all(worker->vm);
	ltasync_open_promise(worker->vm);
	ltasync_open_timer(worker->vm);
	ltasync_open_mainloop(worker->vm);
	_lt_worker_open_disabled_task(worker->vm);

	lt_ImportCtx import_ctx;
	memset(&import_ctx, 0, sizeof(import_ctx));
	if (!_lt_import_callable(&import_ctx, vm, worker->vm, callable, &worker->callable))
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, import_ctx.error[0] ? import_ctx.error : "Failed to import task callable!"));
		_lt_import_ctx_destroy(&import_ctx);
		_lt_worker_destroy_lock(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
		lt_push(vm, promise);
		_lt_release_value(vm, callable);
		_lt_release_value(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	_lt_keep_value(worker->vm, worker->callable);
	_lt_import_ctx_destroy(&import_ctx);

	lt_SharedMarshalCtx marshal_ctx;
	memset(&marshal_ctx, 0, sizeof(marshal_ctx));
	lt_SharedValue shared_state;
	if (!_lt_shared_from_vm(&marshal_ctx, vm, state, &shared_state))
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, marshal_ctx.error[0] ? marshal_ctx.error : "Unsupported value crossing worker boundary!"));
		_lt_shared_ctx_destroy(&marshal_ctx);
		_lt_release_value(worker->vm, worker->callable);
		_lt_worker_destroy_lock(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
		lt_push(vm, promise);
		_lt_release_value(vm, callable);
		_lt_release_value(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	worker->state_arg = _lt_shared_to_vm(worker->vm, &shared_state);
	_lt_keep_value(worker->vm, worker->state_arg);
	_lt_shared_value_clear(&shared_state);
	_lt_shared_ctx_destroy(&marshal_ctx);

#if defined(_WIN32)
	worker->handle = CreateThread(0, 0, _lt_worker_main, worker, 0, 0);
	if (!worker->handle)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker thread!"));
		_lt_release_value(worker->vm, worker->state_arg);
		_lt_release_value(worker->vm, worker->callable);
		_lt_worker_destroy_lock(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
	}
	else lt_buffer_push(vm, &vm->workers, &worker);
#else
	if (pthread_create(&worker->thread, 0, _lt_worker_main, worker) != 0)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker thread!"));
		_lt_release_value(worker->vm, worker->state_arg);
		_lt_release_value(worker->vm, worker->callable);
		_lt_worker_destroy_lock(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
	}
	else lt_buffer_push(vm, &vm->workers, &worker);
#endif

	lt_push(vm, promise);
	_lt_release_value(vm, callable);
	_lt_release_value(vm, state);
	lt_resumecollect(vm, LT_GET_OBJECT(promise));
	return 1;
}

void ltasync_open_all(lt_VM* vm)
{
	ltasync_open_promise(vm);
	ltasync_open_timer(vm);
	ltasync_open_task(vm);
	ltasync_open_mainloop(vm);
}

void ltasync_open_promise(lt_VM* vm)
{
	lt_table_set(vm, vm->global, lt_make_string(vm, "Promise"), lt_make_native(vm, ltasync_native_promise));
}

void ltasync_open_timer(lt_VM* vm)
{
	lt_table_set(vm, vm->global, lt_make_string(vm, "setTimeout"), lt_make_native(vm, ltasync_native_set_timeout));
	lt_table_set(vm, vm->global, lt_make_string(vm, "setInterval"), lt_make_native(vm, ltasync_native_set_interval));
	lt_table_set(vm, vm->global, lt_make_string(vm, "clearTimeout"), lt_make_native(vm, ltasync_native_clear_timer));
	lt_table_set(vm, vm->global, lt_make_string(vm, "clearInterval"), lt_make_native(vm, ltasync_native_clear_timer));
}

void ltasync_open_task(lt_VM* vm)
{
	lt_Value task = lt_make_table(vm);
	lt_table_set(vm, task, lt_make_string(vm, "run"), lt_make_native(vm, ltasync_native_task_run));
	lt_table_set(vm, vm->global, lt_make_string(vm, "task"), task);
}

void ltasync_open_mainloop(lt_VM* vm)
{
	lt_Value mainloop = lt_make_table(vm);
	lt_table_set(vm, mainloop, lt_make_string(vm, "run"), lt_make_native(vm, ltasync_native_mainloop_run));
	lt_table_set(vm, mainloop, lt_make_string(vm, "poll"), lt_make_native(vm, ltasync_native_mainloop_poll));
	lt_table_set(vm, mainloop, lt_make_string(vm, "stop"), lt_make_native(vm, ltasync_native_mainloop_stop));
	lt_table_set(vm, vm->global, lt_make_string(vm, "mainloop"), mainloop);
}
