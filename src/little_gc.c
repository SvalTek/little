#include "little.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"
#include "little_value_internal.h"
#include "little_async_internal.h"

#include <string.h>
lt_Object* lt_allocate(lt_VM* vm, lt_ObjectType type)
{
	lt_Object* obj = vm->alloc(sizeof(lt_Object));
	memset(obj, 0, sizeof(lt_Object));
	obj->type = type;
	obj->debug_name = LT_VALUE_NULL;

	lt_buffer_push(vm, &vm->heap, &obj);

	return obj;
}

void lt_free(lt_VM* vm, uint32_t heapidx)
{
	lt_Object* obj = *(lt_Object**)lt_buffer_at(&vm->heap, heapidx);

	switch (obj->type)
	{
	case LT_OBJECT_CHUNK: {
		lt_buffer_destroy(vm, &obj->chunk.code);
		lt_buffer_destroy(vm, &obj->chunk.constants);
	} break;
	case LT_OBJECT_CLOSURE: {
		lt_buffer_destroy(vm, &obj->closure.captures);
	} break;
	case LT_OBJECT_FN: {
		lt_buffer_destroy(vm, &obj->fn.code);
		lt_buffer_destroy(vm, &obj->fn.constants);
	} break;
	case LT_OBJECT_TABLE: {
		for (uint8_t i = 0; i < 16; ++i)
			lt_buffer_destroy(vm, obj->table.buckets + i);
	} break;
	case LT_OBJECT_ARRAY: {
		lt_buffer_destroy(vm, &obj->array);
	} break;
	case LT_OBJECT_PROMISE: {
		ltasync_free_promise(vm, obj);
	} break;
	case LT_OBJECT_CLASS: {
		lt_table_destroy_internal(vm, &obj->class_def.public_fields);
		lt_table_destroy_internal(vm, &obj->class_def.private_fields);
		lt_table_destroy_internal(vm, &obj->class_def.public_methods);
		lt_table_destroy_internal(vm, &obj->class_def.private_methods);
		lt_table_destroy_internal(vm, &obj->class_def.public_getters);
		lt_table_destroy_internal(vm, &obj->class_def.private_getters);
		lt_table_destroy_internal(vm, &obj->class_def.public_setters);
		lt_table_destroy_internal(vm, &obj->class_def.private_setters);
	} break;
	case LT_OBJECT_INSTANCE: {
		if (obj->instance.native_data && obj->instance.native_data_destroy)
		{
			void* data = obj->instance.native_data;
			obj->instance.native_data = 0;
			obj->instance.native_data_destroy(data);
		}
		lt_table_destroy_internal(vm, &obj->instance.public_fields);
		lt_table_destroy_internal(vm, &obj->instance.private_fields);
	} break;
	case LT_OBJECT_CELL: {
	} break;
	case LT_OBJECT_PTR: {
		vm->free(obj->ptr);
	} break;
	case LT_OBJECT_SHARED_TABLE:
	case LT_OBJECT_SHARED_ARRAY: {
		ltshared_release(obj->shared);
	} break;
	}

	lt_buffer_cycle(&vm->heap, heapidx);
	vm->free(obj);
}

void lt_nocollect(lt_VM* vm, lt_Object* obj)
{
	lt_buffer_push(vm, &vm->keepalive, &obj);
}

void lt_resumecollect(lt_VM* vm, lt_Object* obj)
{
	for (uint32_t i = 0; i < vm->keepalive.length; i++)
	{
		if ((*(lt_Object**)lt_buffer_at(&vm->keepalive, i)) == obj)
		{
			lt_buffer_cycle(&vm->keepalive, i);
			return;
		}
	}
}

#define MARK(x) (x->markbit = 1)
#define CLEAR(x) (x->markbit = 0)

void lt_sweep(lt_VM* vm, lt_Object* obj);

void lt_sweep_v(lt_VM* vm, lt_Value val)
{
	if (LT_IS_OBJECT(val)) lt_sweep(vm, LT_GET_OBJECT(val));
	else if (LT_IS_STRING(val)) lt_reference_string_internal(vm, val);
}

void lt_sweep(lt_VM* vm, lt_Object* obj)
{
	if (!obj || !obj->markbit) return;
	CLEAR(obj);
	switch (obj->type)
	{
	case LT_OBJECT_CHUNK: {
		for (uint32_t i = 0; i < obj->chunk.constants.length; ++i)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->chunk.constants, i));
		}
	} break;
	case LT_OBJECT_CLOSURE: {
		lt_sweep_v(vm, obj->closure.function);
		if (obj->closure.owner_class) lt_sweep(vm, obj->closure.owner_class);
		for (uint32_t i = 0; i < obj->closure.captures.length; ++i)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->closure.captures, i));
		}
	} break;
	case LT_OBJECT_CELL: {
		lt_sweep_v(vm, obj->cell);
	} break;
	case LT_OBJECT_BOUND_NATIVE: {
		lt_sweep_v(vm, obj->bound_native.receiver);
		lt_sweep_v(vm, obj->debug_name);
	} break;
	case LT_OBJECT_NATIVEFN: {
		lt_sweep_v(vm, obj->debug_name);
	} break;
	case LT_OBJECT_FN: {
		if (obj->fn.owner_class) lt_sweep(vm, obj->fn.owner_class);
		for (uint32_t i = 0; i < obj->fn.constants.length; ++i)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->fn.constants, i));
		}
	} break;
	case LT_OBJECT_TABLE: {
		lt_sweep_v(vm, obj->debug_name);
		for (uint16_t i = 0; i < 16; ++i)
		{
			lt_Buffer* bucket = obj->table.buckets + i;
			for (uint32_t j = 0; j < bucket->length; ++j)
			{
				lt_sweep_v(vm, ((lt_TablePair*)lt_buffer_at(bucket, j))->key);
				lt_sweep_v(vm, ((lt_TablePair*)lt_buffer_at(bucket, j))->value);
			}
		}
	} break;
	case LT_OBJECT_ARRAY: {
		for (uint32_t j = 0; j < obj->array.length; ++j)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->array, j));
		}
	} break;
	case LT_OBJECT_PROMISE: {
		ltasync_mark_promise(vm, obj);
	} break;
	case LT_OBJECT_CLASS: {
		if (obj->class_def.superclass) lt_sweep(vm, obj->class_def.superclass);
		lt_sweep_v(vm, obj->class_def.name);
		lt_sweep_v(vm, obj->class_def.constructor);
		lt_table_mark_internal(vm, &obj->class_def.public_fields);
		lt_table_mark_internal(vm, &obj->class_def.private_fields);
		lt_table_mark_internal(vm, &obj->class_def.public_methods);
		lt_table_mark_internal(vm, &obj->class_def.private_methods);
		lt_table_mark_internal(vm, &obj->class_def.public_getters);
		lt_table_mark_internal(vm, &obj->class_def.private_getters);
		lt_table_mark_internal(vm, &obj->class_def.public_setters);
		lt_table_mark_internal(vm, &obj->class_def.private_setters);
	} break;
	case LT_OBJECT_INSTANCE: {
		lt_sweep(vm, obj->instance.klass);
		lt_table_mark_internal(vm, &obj->instance.public_fields);
		lt_table_mark_internal(vm, &obj->instance.private_fields);
	} break;
	case LT_OBJECT_SHARED_TABLE:
	case LT_OBJECT_SHARED_ARRAY: {
	} break;
	}
}

uint32_t lt_collect(lt_VM* vm)
{
	uint32_t num_collected = 0;

	for (uint32_t i = 0; i < vm->heap.length; ++i)
	{
		lt_Object* obj = *(lt_Object**)lt_buffer_at(&vm->heap, i);
		MARK(obj);
	}

	for (uint32_t i = 0; i < LT_DEDUP_TABLE_SIZE; i++)
	{
		for (uint32_t j = 0; j < vm->strings[i].length; ++j)
		{
			lt_StringDedupEntry* e = lt_buffer_at(vm->strings + i, j);
			e->refcount = 0;
		}
	}

	for (uint32_t i = 0; i < vm->keepalive.length; ++i)
	{
		lt_sweep(vm, *(lt_Object**)lt_buffer_at(&vm->keepalive, i));
	}
	for (uint32_t i = 0; i < vm->temporary_roots.length; ++i)
	{
		lt_sweep(vm, *(lt_Object**)lt_buffer_at(&vm->temporary_roots, i));
	}
	for (uint32_t i = 0; i < vm->native_roots.length; ++i)
	{
		lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&vm->native_roots, i));
	}

	for (uint32_t i = 0; i < vm->top; ++i)
	{
		lt_sweep_v(vm, vm->stack[i]);
	}

	ltasync_mark_roots(vm);

	for (uint32_t i = 0; i < vm->heap.length; ++i)
	{
		lt_Object* obj = *(lt_Object**)lt_buffer_at(&vm->heap, i);
		if (obj->markbit)
		{
			lt_free(vm, i--);
			num_collected++;
		}
	}

	for (uint32_t i = 0; i < LT_DEDUP_TABLE_SIZE; i++)
	{
		for (uint32_t j = 0; j < vm->strings[i].length; ++j)
		{
			lt_StringDedupEntry* e = lt_buffer_at(vm->strings + i, j);
			if (e->refcount == 0 && e->hash != 0)
			{
				vm->free(e->string);
				e->hash = 0; // mark for reopen
			}
		}
	}

	return num_collected;
}
