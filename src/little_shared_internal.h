#pragma once

#include "little.h"

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
#endif

#ifndef LT_TASK_SHARED_MAX_NODES
#define LT_TASK_SHARED_MAX_NODES 4096
#endif

#ifndef LT_TASK_SHARED_MAX_DEPTH
#define LT_TASK_SHARED_MAX_DEPTH 256
#endif

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

void ltshared_retain(lt_SharedObject* shared);
void ltshared_release(lt_SharedObject* shared);
lt_Value ltshared_make_proxy(lt_VM* vm, lt_SharedObject* shared);
lt_Value ltshared_table_get(lt_VM* vm, lt_SharedObject* shared, lt_Value key);
lt_Value ltshared_table_set(lt_VM* vm, lt_SharedObject* shared, lt_Value key, lt_Value val);
uint8_t ltshared_table_next(lt_VM* vm, lt_SharedObject* shared, uint64_t* cursor, lt_Value* key, lt_Value* val);
lt_Value ltshared_table_keys(lt_VM* vm, lt_SharedObject* shared);
lt_Value ltshared_table_values(lt_VM* vm, lt_SharedObject* shared);
lt_Value ltshared_array_get(lt_VM* vm, lt_SharedObject* shared, uint32_t idx);
lt_Value ltshared_array_set(lt_VM* vm, lt_SharedObject* shared, uint32_t idx, lt_Value val);
lt_Value ltshared_array_push(lt_VM* vm, lt_SharedObject* shared, lt_Value val);
lt_Value ltshared_array_remove(lt_VM* vm, lt_SharedObject* shared, uint32_t idx);
uint32_t ltshared_array_length(lt_SharedObject* shared);

void lt_shared_value_clear_internal(lt_SharedValue* value);
void lt_shared_value_retain_external_internal(lt_SharedValue* value);
void lt_shared_value_release_external_internal(lt_SharedValue* value);
uint8_t lt_shared_reserve_internal(void** data, uint32_t* capacity, uint32_t length, uint32_t element_size);
void lt_shared_ctx_destroy_internal(lt_SharedMarshalCtx* ctx);
uint8_t lt_shared_from_vm_internal(lt_SharedMarshalCtx* ctx, lt_VM* vm, lt_Value value, lt_SharedValue* out);
lt_Value lt_shared_to_vm_internal(lt_VM* vm, lt_SharedValue* value);
