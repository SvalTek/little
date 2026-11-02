#include "little_async.h"
#include "little_task_internal.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void _lt_worker_error(lt_VM* vm, const char* msg)
{
	lt_Worker* worker = vm->error_context;
	if (!worker) return;

	ltasync_worker_set_error_internal(worker, msg);
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
	if (!lt_shared_reserve_internal((void**)&ctx->entries, &ctx->capacity, ctx->length, sizeof(lt_ImportEntry))) return 0;
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
	ltasync_keep_value_internal(vm, worker->callable);
	lt_push(vm, worker->state_arg);
	ltasync_release_value_internal(vm, worker->state_arg);
	ltasync_release_value_internal(vm, worker->callable);
	uint32_t nret = lt_exec(vm, worker->callable, 1);
	ltasync_release_value_internal(vm, worker->callable);

	ltasync_worker_lock_internal(worker);
	uint8_t has_error = worker->error != 0;
	ltasync_worker_unlock_internal(worker);

	if (!has_error)
	{
		lt_Value value = lt_pack_values_internal(vm, (uint16_t)nret);
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
				ltasync_worker_set_error_internal(worker, "Task promise was not resolved!");
			}
			else if (promise->promise.state == LT_PROMISE_REJECTED)
			{
				if (LT_IS_STRING(promise->promise.result)) ltasync_worker_set_error_internal(worker, lt_get_string(vm, promise->promise.result));
				else ltasync_worker_set_error_internal(worker, "Task promise rejected!");
			}
			else value = promise->promise.result;
			lt_resumecollect(vm, promise);
		}

		ltasync_worker_lock_internal(worker);
		has_error = worker->error != 0;
		ltasync_worker_unlock_internal(worker);

		if (!has_error)
		{
			lt_SharedMarshalCtx ctx;
			memset(&ctx, 0, sizeof(ctx));
			if (lt_shared_from_vm_internal(&ctx, vm, value, &worker->result))
			{
				lt_shared_value_retain_external_internal(&worker->result);
				ltasync_worker_lock_internal(worker);
				worker->has_result = 1;
				ltasync_worker_unlock_internal(worker);
			}
			else ltasync_worker_set_error_internal(worker, ctx.error[0] ? ctx.error : "Unsupported task result crossing worker boundary!");
			lt_shared_ctx_destroy_internal(&ctx);
		}
	}

	lt_destroy(vm);
	worker->vm = 0;
	ltasync_worker_mark_done_internal(worker);
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
	if (!ltasync_is_callable(callable) || LT_IS_NATIVE(callable) ||
		(LT_IS_OBJECT(callable) && LT_GET_OBJECT(callable)->type == LT_OBJECT_BOUND_NATIVE))
		lt_runtime_error(vm, "Expected task.run callable to be a Little function!");

	lt_Value promise = ltasync_make_promise(vm);
	lt_nocollect(vm, LT_GET_OBJECT(promise));
	ltasync_keep_value_internal(vm, state);
	ltasync_keep_value_internal(vm, callable);

	lt_Worker* worker = vm->alloc(sizeof(lt_Worker));
	if (!worker)
	{
		ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to allocate worker!"));
		lt_push(vm, promise);
		ltasync_release_value_internal(vm, callable);
		ltasync_release_value_internal(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	memset(worker, 0, sizeof(lt_Worker));
	ltasync_worker_init_lock_internal(worker);
	worker->parent = vm;
	worker->promise = LT_GET_OBJECT(promise);
	worker->state = state;
	worker->callee = callable;

	worker->vm = lt_open(malloc, free, _lt_worker_error);
	if (!worker->vm)
	{
		ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker VM!"));
		ltasync_worker_destroy_lock_internal(worker);
		vm->free(worker);
		lt_push(vm, promise);
		ltasync_release_value_internal(vm, callable);
		ltasync_release_value_internal(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	worker->vm->error_context = worker;
	if (vm->worker_vm_init) vm->worker_vm_init(worker->vm);
	ltasync_open_promise(worker->vm);
	ltasync_open_timer(worker->vm);
	ltasync_open_mainloop(worker->vm);
	_lt_worker_open_disabled_task(worker->vm);

	lt_ImportCtx import_ctx;
	memset(&import_ctx, 0, sizeof(import_ctx));
	if (!_lt_import_callable(&import_ctx, vm, worker->vm, callable, &worker->callable))
	{
		ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, import_ctx.error[0] ? import_ctx.error : "Failed to import task callable!"));
		_lt_import_ctx_destroy(&import_ctx);
		ltasync_worker_destroy_lock_internal(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
		lt_push(vm, promise);
		ltasync_release_value_internal(vm, callable);
		ltasync_release_value_internal(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	ltasync_keep_value_internal(worker->vm, worker->callable);
	_lt_import_ctx_destroy(&import_ctx);

	lt_SharedMarshalCtx marshal_ctx;
	memset(&marshal_ctx, 0, sizeof(marshal_ctx));
	lt_SharedValue shared_state;
	if (!lt_shared_from_vm_internal(&marshal_ctx, vm, state, &shared_state))
	{
		ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, marshal_ctx.error[0] ? marshal_ctx.error : "Unsupported value crossing worker boundary!"));
		lt_shared_ctx_destroy_internal(&marshal_ctx);
		ltasync_release_value_internal(worker->vm, worker->callable);
		ltasync_worker_destroy_lock_internal(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
		lt_push(vm, promise);
		ltasync_release_value_internal(vm, callable);
		ltasync_release_value_internal(vm, state);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}
	worker->state_arg = lt_shared_to_vm_internal(worker->vm, &shared_state);
	ltasync_keep_value_internal(worker->vm, worker->state_arg);
	lt_shared_value_clear_internal(&shared_state);
	lt_shared_ctx_destroy_internal(&marshal_ctx);

#if defined(_WIN32)
	worker->handle = CreateThread(0, 0, _lt_worker_main, worker, 0, 0);
	if (!worker->handle)
	{
		ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker thread!"));
		ltasync_release_value_internal(worker->vm, worker->state_arg);
		ltasync_release_value_internal(worker->vm, worker->callable);
		ltasync_worker_destroy_lock_internal(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
	}
	else lt_buffer_push(vm, &vm->workers, &worker);
#else
	if (pthread_create(&worker->thread, 0, _lt_worker_main, worker) != 0)
	{
		ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker thread!"));
		ltasync_release_value_internal(worker->vm, worker->state_arg);
		ltasync_release_value_internal(worker->vm, worker->callable);
		ltasync_worker_destroy_lock_internal(worker);
		lt_destroy(worker->vm);
		vm->free(worker);
	}
	else lt_buffer_push(vm, &vm->workers, &worker);
#endif

	lt_push(vm, promise);
	ltasync_release_value_internal(vm, callable);
	ltasync_release_value_internal(vm, state);
	lt_resumecollect(vm, LT_GET_OBJECT(promise));
	return 1;
}
