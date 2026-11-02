#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint64_t lt_Value;

#define LT_SIGN_BIT   (0b1000000000000000000000000000000000000000000000000000000000000000)
#define LT_EXPONENT   (0b0111111111110000000000000000000000000000000000000000000000000000)
#define LT_QNAN_BIT   (0b0000000000001000000000000000000000000000000000000000000000000000)
#define LT_TYPE_MASK  (0b0000000000000111000000000000000000000000000000000000000000000000)
#define LT_VALUE_MASK (0b0000000000000000111111111111111111111111111111111111111111111111)

#define LT_NAN_MASK (LT_EXPONENT | LT_QNAN_BIT)

#define LT_TYPE_NULL    (0b0000000000000011000000000000000000000000000000000000000000000000)
#define LT_TYPE_BOOL    (0b0000000000000001000000000000000000000000000000000000000000000000)
#define LT_TYPE_STRING  (0b0000000000000010000000000000000000000000000000000000000000000000)
#define LT_TYPE_OBJECT  (0b0000000000000101000000000000000000000000000000000000000000000000)

#define LT_VALUE_NULL       ((lt_Value)(LT_NAN_MASK | LT_TYPE_NULL))
#define LT_VALUE_FALSE      ((lt_Value)(LT_NAN_MASK | LT_TYPE_BOOL))
#define LT_VALUE_TRUE       ((lt_Value)(LT_NAN_MASK | (LT_TYPE_BOOL | 1)))
#define LT_VALUE_NUMBER(x)  ((lt_Value)(lt_make_number((double)x)))
#define LT_VALUE_OBJECT(x)  ((lt_Value)(LT_NAN_MASK | (LT_TYPE_OBJECT | (uint64_t)x)))

#ifndef LT_STACK_SIZE
#define LT_STACK_SIZE 256
#endif

#ifndef LT_CALLSTACK_SIZE
#define LT_CALLSTACK_SIZE 32
#endif

#ifndef LT_DEDUP_TABLE_SIZE
#define LT_DEDUP_TABLE_SIZE 64
#endif

#ifndef LT_MAX_FUNCTION_PARAMS
#define LT_MAX_FUNCTION_PARAMS 16
#endif

#ifndef LT_MAX_CALL_ARGS
#define LT_MAX_CALL_ARGS 16
#endif

#define LT_MAX_LOOP_VARS 2

#ifndef LT_MAX_BRANCHES
#define LT_MAX_BRANCHES 32
#endif

#ifndef LT_MAX_RETURNS
#define LT_MAX_RETURNS 255
#endif

#ifndef LT_MAX_CONSTANTS
#define LT_MAX_CONSTANTS 32767
#endif

#ifndef LT_MAX_LOCALS
#define LT_MAX_LOCALS (LT_STACK_SIZE - 1)
#endif

#define LT_IS_NUMBER(x)  (((x) & LT_NAN_MASK) != LT_NAN_MASK)
#define LT_IS_NULL(x)    ((x) == LT_VALUE_NULL)
#define LT_IS_BOOL(x)    (x == LT_VALUE_TRUE || x == LT_VALUE_FALSE)
#define LT_IS_TRUE(x)    (x == LT_VALUE_TRUE)
#define LT_IS_FALSE(x)   (x == LT_VALUE_FALSE)
#define LT_IS_TRUTHY(x) (!(x == LT_VALUE_FALSE || x == LT_VALUE_NULL))
#define LT_IS_STRING(x)  (!LT_IS_NUMBER(x) && (x & LT_TYPE_MASK) == LT_TYPE_STRING)
#define LT_IS_OBJECT(x)  (!LT_IS_NUMBER(x) && (x & LT_TYPE_MASK) == LT_TYPE_OBJECT)
#define LT_IS_TABLE(x)    (LT_IS_OBJECT(x) && (LT_GET_OBJECT(x)->type == LT_OBJECT_TABLE || LT_GET_OBJECT(x)->type == LT_OBJECT_SHARED_TABLE))
#define LT_IS_ARRAY(x)    (LT_IS_OBJECT(x) && (LT_GET_OBJECT(x)->type == LT_OBJECT_ARRAY || LT_GET_OBJECT(x)->type == LT_OBJECT_SHARED_ARRAY))
#define LT_IS_FUNCTION(x) (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_FN)
#define LT_IS_CLOSURE(x)  (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_CLOSURE)
#define LT_IS_NATIVE(x)   (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_NATIVEFN)
#define LT_IS_CLASS(x)    (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_CLASS)
#define LT_IS_INSTANCE(x) (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_INSTANCE)
#define LT_IS_PTR(x)      (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_PTR)
#define LT_IS_CELL(x)     (LT_IS_OBJECT(x) && LT_GET_OBJECT(x)->type == LT_OBJECT_CELL)

#define LT_GET_NUMBER(x) lt_get_number(x)
#define LT_GET_STRING(vm, x) lt_get_string(vm, x)
#define LT_GET_OBJECT(x) ((lt_Object*)(x & LT_VALUE_MASK))

typedef struct {
	void* data;
	uint32_t length, capacity, element_size;
} lt_Buffer;

typedef struct
{
	uint16_t line, col;
} lt_DebugLoc;

typedef struct
{
	const char* module_name;
	lt_Buffer locations;
} lt_DebugInfo;

typedef enum {
	LT_VIS_PUBLIC,
	LT_VIS_PRIVATE,
} lt_Visibility;

typedef struct {
	lt_Value key, value;
} lt_TablePair;

typedef struct {
	lt_Buffer buckets[16];
} lt_Table;

typedef enum {
	LT_PROMISE_PENDING,
	LT_PROMISE_FULFILLED,
	LT_PROMISE_REJECTED,
} lt_PromiseState;

typedef enum {
	LT_OBJECT_CHUNK,
	LT_OBJECT_FN,
	LT_OBJECT_CLOSURE,
	LT_OBJECT_TABLE,
	LT_OBJECT_ARRAY,
	LT_OBJECT_NATIVEFN,
	LT_OBJECT_BOUND_NATIVE,
	LT_OBJECT_PROMISE,
	LT_OBJECT_CLASS,
	LT_OBJECT_INSTANCE,
	LT_OBJECT_CELL,
	LT_OBJECT_PTR,
	LT_OBJECT_SHARED_TABLE,
	LT_OBJECT_SHARED_ARRAY,
} lt_ObjectType;

typedef struct lt_VM lt_VM;
typedef struct lt_SharedObject lt_SharedObject;
typedef struct lt_Api lt_Api;

typedef uint8_t(*lt_NativeFn)(lt_VM* vm, uint8_t argc);
/* Called during instance finalization. Must only release native resources; it
   must not call back into Little, allocate VM values, or trigger collection. */
typedef void(*lt_NativeDataDestroyFn)(void* data);

typedef struct lt_Object {
	lt_ObjectType type;

	/* Debug label for native functions and tables: the first string key the
	   value is stored under, used in tracebacks. LT_VALUE_NULL for other
	   object types. */
	lt_Value debug_name;

	union
	{
		struct
		{
			lt_Buffer code;
			lt_Buffer constants;
			char* name;
			lt_DebugInfo* debug;
		} chunk;

		struct
		{
			uint8_t arity;
			uint8_t is_async;
			struct lt_Object* owner_class;
			lt_Buffer code;
			lt_Buffer constants;
			lt_DebugInfo* debug;
		} fn;

		struct
		{
			char* string;
			uint16_t len;
		} string;

		struct
		{
			lt_Value function;
			lt_Buffer captures;
			struct lt_Object* owner_class;
		} closure;

		struct
		{
			lt_NativeFn native;
			lt_Value receiver;
		} bound_native;

		lt_Table table;
		lt_Buffer array;
		lt_NativeFn native;
		struct
		{
			lt_PromiseState state;
			lt_Value result;
			lt_Buffer reactions;
			uint8_t handled;
		} promise;
		struct
		{
			lt_Value name;
			lt_Table public_fields;
			lt_Table private_fields;
			lt_Table public_methods;
			lt_Table private_methods;
			lt_Table public_getters;
			lt_Table private_getters;
			lt_Table public_setters;
			lt_Table private_setters;
			lt_Value constructor;
			struct lt_Object* superclass;
			lt_NativeDataDestroyFn native_data_destroy;
			uint8_t is_native_class;
		} class_def;
		struct
		{
			struct lt_Object* klass;
			lt_Table public_fields;
			lt_Table private_fields;
			void* native_data;
			lt_NativeDataDestroyFn native_data_destroy;
		} instance;
		lt_Value cell;
		void* ptr;
		lt_SharedObject* shared;
	};

	uint8_t markbit : 1;
} lt_Object;
