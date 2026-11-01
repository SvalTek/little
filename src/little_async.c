#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "little_async.h"
#include "little.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"
#include "little_async_internal.h"
#include "little_task_internal.h"
#include "little_runloop_internal.h"
#include "little_shared_internal.h"

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

static uint8_t _lt_promise_next(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_catch(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_finally(lt_VM* vm, uint8_t argc);
void ltasync_worker_lock_internal(lt_Worker* worker)
{
#if defined(_WIN32)
	EnterCriticalSection(&worker->lock);
#else
	pthread_mutex_lock(&worker->lock);
#endif
}

void ltasync_worker_unlock_internal(lt_Worker* worker)
{
#if defined(_WIN32)
	LeaveCriticalSection(&worker->lock);
#else
	pthread_mutex_unlock(&worker->lock);
#endif
}

void ltasync_worker_init_lock_internal(lt_Worker* worker)
{
#if defined(_WIN32)
	InitializeCriticalSection(&worker->lock);
#else
	pthread_mutex_init(&worker->lock, 0);
#endif
}

void ltasync_worker_destroy_lock_internal(lt_Worker* worker)
{
#if defined(_WIN32)
	DeleteCriticalSection(&worker->lock);
#else
	pthread_mutex_destroy(&worker->lock);
#endif
}


static uint8_t _lt_worker_is_done(lt_Worker* worker)
{
	ltasync_worker_lock_internal(worker);
	uint8_t done = worker->done;
	ltasync_worker_unlock_internal(worker);
	return done;
}

void ltasync_worker_mark_done_internal(lt_Worker* worker)
{
	ltasync_worker_lock_internal(worker);
	worker->done = 1;
	ltasync_worker_unlock_internal(worker);
}

void ltasync_worker_set_error_internal(lt_Worker* worker, const char* msg)
{
	ltasync_worker_lock_internal(worker);
	if (!worker->error)
	{
		uint32_t len = (uint32_t)strlen(msg);
		worker->error = malloc(len + 1);
		if (worker->error) memcpy(worker->error, msg, len + 1);
	}
	ltasync_worker_unlock_internal(worker);
}

uint64_t ltasync_now_ms_internal(void)
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
	struct timespec delay = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&delay, 0);
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

lt_Value ltasync_make_bound_native_internal(lt_VM* vm, lt_NativeFn fn, lt_Value receiver)
{
	lt_Object* bound = lt_allocate(vm, LT_OBJECT_BOUND_NATIVE);
	bound->bound_native.native = fn;
	bound->bound_native.receiver = receiver;
	return LT_VALUE_OBJECT(bound);
}

lt_Value ltasync_current_receiver_internal(lt_VM* vm)
{
	if (!vm->current || !vm->current->callee || vm->current->callee->type != LT_OBJECT_BOUND_NATIVE)
		return LT_VALUE_NULL;
	return vm->current->callee->bound_native.receiver;
}

uint8_t ltasync_is_callable(lt_Value value)
{
	return LT_IS_FUNCTION(value) || LT_IS_CLOSURE(value) || LT_IS_NATIVE(value) ||
		(LT_IS_OBJECT(value) && LT_GET_OBJECT(value)->type == LT_OBJECT_BOUND_NATIVE);
}

void ltasync_keep_value_internal(lt_VM* vm, lt_Value value);
void ltasync_release_value_internal(lt_VM* vm, lt_Value value);

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
	ltasync_keep_value_internal(vm, promise_value);
	ltasync_keep_value_internal(vm, reaction.callback);
	ltasync_keep_value_internal(vm, reaction.next_promise);
	if (promise->promise.state == LT_PROMISE_PENDING)
		lt_buffer_push(vm, &promise->promise.reactions, &reaction);
	else
		_lt_schedule_reaction(vm, promise_value, reaction);
	ltasync_release_value_internal(vm, reaction.next_promise);
	ltasync_release_value_internal(vm, reaction.callback);
	ltasync_release_value_internal(vm, promise_value);
}

void ltasync_keep_value_internal(lt_VM* vm, lt_Value value)
{
	if (LT_IS_OBJECT(value)) lt_nocollect(vm, LT_GET_OBJECT(value));
}

void ltasync_release_value_internal(lt_VM* vm, lt_Value value)
{
	if (LT_IS_OBJECT(value)) lt_resumecollect(vm, LT_GET_OBJECT(value));
}

/**
 * Initializes the asynchronous runtime state for a virtual machine.
 *
 * @param vm Virtual machine whose asynchronous state is initialized.
 */
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
	ltasync_worker_destroy_lock_internal(worker);
	if (worker->vm) lt_destroy(worker->vm);
	if (worker->error) free(worker->error);
	if (worker->has_result)
	{
		lt_shared_value_release_external_internal(&worker->result);
		lt_shared_value_clear_internal(&worker->result);
	}
	vm->free(worker);
}

/**
 * Destroys the asynchronous runtime state associated with a VM.
 *
 * @param vm VM whose workers and asynchronous runtime buffers are destroyed.
 */
void ltasync_destroy_state(lt_VM* vm)
{
	for (uint32_t i = 0; i < vm->poll_hooks.length; ++i)
	{
		lt_PollHookEntry* entry = lt_buffer_at(&vm->poll_hooks, i);
		if (entry->destroy) entry->destroy(vm, entry->context);
	}
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

/**
 * Registers a callback to be invoked during event-loop polling.
 * @param vm VM that owns the poll hook.
 * @param hook Callback to invoke.
 * @param context User-defined context passed to the callback.
 * @returns A nonzero hook identifier, or 0 if hook is NULL.
 */

uint32_t ltasync_add_owned_poll_hook(lt_VM* vm, lt_PollHook hook, void* context, void (*destroy)(lt_VM*, void*))
{
	if (!hook) return 0;
	uint32_t id = vm->next_poll_hook_id++;
	if (id == 0) id = vm->next_poll_hook_id++;
	lt_PollHookEntry entry = { id, hook, context, 0, destroy };
	lt_buffer_push(vm, &vm->poll_hooks, &entry);
	return id;
}

/**
 * Marks a registered poll hook for removal.
 * @param vm Virtual machine containing the poll hook.
 * @param hook_id Identifier of the poll hook to remove.
 */

/**
 * Dispatches the poll hooks that were registered at the start of the poll cycle.
 * @param vm Virtual machine whose poll hooks are dispatched.
 * @param pending Set to 1 when a hook reports work or pending activity.
 * @returns 1 if any hook reports work, 0 otherwise.
 */
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
		uint32_t root_mark = vm->native_roots.length;
		uint32_t saved_root_floor = vm->native_root_floor;
		vm->native_root_floor = root_mark;
		lt_PollResult result = entry.hook(vm, entry.context);
		vm->native_roots.length = root_mark;
		vm->native_root_floor = saved_root_floor;
		if (result == LT_POLL_WORK) did_work = 1;
		if (result == LT_POLL_WORK || result == LT_POLL_PENDING) *pending = 1;
	}

	for (uint32_t i = 0; i < vm->poll_hooks.length; ++i)
	{
		lt_PollHookEntry* entry = lt_buffer_at(&vm->poll_hooks, i);
		if (entry->removed)
		{
			if (entry->destroy) entry->destroy(vm, entry->context);
			lt_buffer_cycle(&vm->poll_hooks, i--);
		}
	}
	return did_work;
}

/**
 * Releases the reaction storage associated with a promise.
 * @param vm VM that owns the promise.
 * @param promise Promise whose reactions are being released.
 */
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

/**
 * Processes one pending asynchronous runtime task or timer and updates the event-loop state.
 *
 * @returns `1` if work was performed or pending activity remains, `0` otherwise.
 */
uint8_t ltasync_poll_internal(lt_VM* vm, int allow_sleep)
{
	uint8_t did_work = 0;
	uint8_t has_pending = 0;
	uint8_t hook_pending = 0;
	uint8_t has_next_timer = 0;
	uint64_t next_timer_due = 0;
	did_work = ltasync_poll_hooks(vm, &hook_pending);
	has_pending = hook_pending;

	if (vm->async_calls.length > 0)
	{
		lt_AsyncCall task = *(lt_AsyncCall*)lt_buffer_at(&vm->async_calls, 0);
		lt_buffer_cycle(&vm->async_calls, 0);
		ltasync_keep_value_internal(vm, task.promise);
		ltasync_keep_value_internal(vm, task.callee);
		for (uint8_t i = 0; i < task.argc; ++i) ltasync_keep_value_internal(vm, task.args[i]);

		for (uint8_t i = 0; i < task.argc; ++i) lt_push(vm, task.args[i]);
		uint16_t nret = lt_exec(vm, task.callee, task.argc);
		lt_Value result = LT_VALUE_NULL;
		if (nret > 0)
		{
			result = lt_pop(vm);
			for (uint16_t i = 1; i < nret; ++i) lt_pop(vm);
		}

		_lt_settle_promise(vm, task.promise, LT_PROMISE_FULFILLED, result);
		for (uint8_t i = task.argc; i > 0; --i) ltasync_release_value_internal(vm, task.args[i - 1]);
		ltasync_release_value_internal(vm, task.callee);
		ltasync_release_value_internal(vm, task.promise);
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
		ltasync_keep_value_internal(vm, task.promise);
		ltasync_keep_value_internal(vm, result);
		ltasync_keep_value_internal(vm, task.reaction.callback);
		ltasync_keep_value_internal(vm, task.reaction.next_promise);

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

		ltasync_release_value_internal(vm, task.reaction.next_promise);
		ltasync_release_value_internal(vm, task.reaction.callback);
		ltasync_release_value_internal(vm, result);
		ltasync_release_value_internal(vm, task.promise);
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
			_lt_settle_promise(vm, promise, LT_PROMISE_FULFILLED, worker->has_result ? lt_shared_to_vm_internal(vm, &worker->result) : LT_VALUE_NULL);

		_lt_worker_destroy(vm, worker, 1);
		lt_buffer_cycle(&vm->workers, i--);
		return 1;
	}

	uint64_t now = ltasync_now_ms_internal();

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
					current->due_ms = ltasync_now_ms_internal() + current->interval_ms;
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
			if ((vm->workers.length > 0 || hook_pending) && until_due > 10) until_due = 10;
			if (until_due > UINT32_MAX) until_due = UINT32_MAX;
			sleep_ms = (uint32_t)until_due;
		}
		if (sleep_ms == 0) sleep_ms = 1;
		if (allow_sleep) _lt_sleep_ms(sleep_ms);
	}
	return did_work || has_pending || vm->async_calls.length > 0 || vm->microtasks.length > 0 || vm->workers.length > 0;
}


static uint8_t _lt_add_promise_callback(lt_VM* vm, uint8_t argc, lt_ReactionKind kind)
{
	if (argc != 1) lt_runtime_error(vm, "Expected one promise callback!");
	lt_Value callback = lt_pop(vm);
	if (!ltasync_is_callable(callback)) lt_runtime_error(vm, "Expected promise callback to be callable!");

	lt_Value promise = ltasync_current_receiver_internal(vm);
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
	if (strcmp(method, "next") == 0) return ltasync_make_bound_native_internal(vm, _lt_promise_next, promise);
	if (strcmp(method, "catch") == 0) return ltasync_make_bound_native_internal(vm, _lt_promise_catch, promise);
	if (strcmp(method, "finally") == 0) return ltasync_make_bound_native_internal(vm, _lt_promise_finally, promise);
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

lt_Value ltasync_make_promise(lt_VM* vm)
{
    return _lt_make_promise(vm);
}

void ltasync_settle_promise(lt_VM* vm, lt_Value promise, lt_PromiseState state, lt_Value result)
{
    _lt_settle_promise(vm, promise, state, result);
}
