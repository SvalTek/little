#include "little_async.h"
#include "little_internal.h"
#include "little_std.h"

#include <ctype.h>
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

typedef struct {
	uint32_t id;
	uint64_t due_ms;
	uint64_t interval_ms;
	lt_Value callback;
	uint8_t repeat;
	uint8_t cancelled;
} lt_Timer;

typedef struct {
	lt_VM* parent;
	lt_Object* promise;
	char* source;
	char* state_literal;
	char* result_literal;
	lt_Value state;
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
static lt_Value _lt_eval_literal(lt_VM* vm, const char* literal);

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
	if (promise->promise.state == LT_PROMISE_PENDING)
		lt_buffer_push(vm, &promise->promise.reactions, &reaction);
	else
		_lt_schedule_reaction(vm, promise_value, reaction);
}

static void _lt_str_append(lt_VM* vm, lt_Buffer* out, const char* text)
{
	while (*text)
	{
		char c = *text++;
		lt_buffer_push(vm, out, &c);
	}
}

static void _lt_str_append_char(lt_VM* vm, lt_Buffer* out, char c)
{
	lt_buffer_push(vm, out, &c);
}

static uint8_t _lt_is_identifier_string(const char* str)
{
	if (!str || (!isalpha((unsigned char)*str) && *str != '_')) return 0;
	while (*++str)
		if (!isalnum((unsigned char)*str) && *str != '_') return 0;
	return 1;
}

static uint8_t _lt_serialize_value(lt_VM* vm, lt_Value value, lt_Buffer* out, char* error, uint32_t error_size);

static void _lt_serialize_string(lt_VM* vm, const char* str, lt_Buffer* out)
{
	_lt_str_append_char(vm, out, '"');
	while (*str)
	{
		if (*str == '"' || *str == '\\') _lt_str_append_char(vm, out, '\\');
		_lt_str_append_char(vm, out, *str++);
	}
	_lt_str_append_char(vm, out, '"');
}

static uint8_t _lt_serialize_value(lt_VM* vm, lt_Value value, lt_Buffer* out, char* error, uint32_t error_size)
{
	char scratch[64];
	if (LT_IS_NULL(value)) _lt_str_append(vm, out, "null");
	else if (LT_IS_TRUE(value)) _lt_str_append(vm, out, "true");
	else if (LT_IS_FALSE(value)) _lt_str_append(vm, out, "false");
	else if (LT_IS_NUMBER(value))
	{
		snprintf(scratch, sizeof(scratch), "%.17g", lt_get_number(value));
		_lt_str_append(vm, out, scratch);
	}
	else if (LT_IS_STRING(value))
	{
		_lt_serialize_string(vm, lt_get_string(vm, value), out);
	}
	else if (LT_IS_ARRAY(value))
	{
		_lt_str_append_char(vm, out, '[');
		for (uint32_t i = 0; i < lt_array_length(value); ++i)
		{
			if (i > 0) _lt_str_append_char(vm, out, ',');
			if (!_lt_serialize_value(vm, *lt_array_at(value, i), out, error, error_size)) return 0;
		}
		_lt_str_append_char(vm, out, ']');
	}
	else if (LT_IS_TABLE(value))
	{
		_lt_str_append_char(vm, out, '{');
		lt_Object* table = LT_GET_OBJECT(value);
		uint8_t first = 1;
		for (uint32_t bucket = 0; bucket < 16; ++bucket)
		{
			lt_Buffer* pairs = table->table.buckets + bucket;
			for (uint32_t i = 0; i < pairs->length; ++i)
			{
				lt_TablePair* pair = lt_buffer_at(pairs, i);
				if (!first) _lt_str_append_char(vm, out, ' ');
				first = 0;

				if (LT_IS_STRING(pair->key) && _lt_is_identifier_string(lt_get_string(vm, pair->key)))
					_lt_str_append(vm, out, lt_get_string(vm, pair->key));
				else if (!_lt_serialize_value(vm, pair->key, out, error, error_size))
					return 0;

				_lt_str_append_char(vm, out, ':');
				if (!_lt_serialize_value(vm, pair->value, out, error, error_size)) return 0;
			}
		}
		_lt_str_append_char(vm, out, '}');
	}
	else
	{
		snprintf(error, error_size, "Unsupported value crossing worker boundary!");
		return 0;
	}

	return 1;
}

static char* _lt_serialize_to_string(lt_VM* vm, lt_Value value, char* error, uint32_t error_size)
{
	lt_Buffer out = lt_buffer_new(sizeof(char));
	if (!_lt_serialize_value(vm, value, &out, error, error_size))
	{
		lt_buffer_destroy(vm, &out);
		return 0;
	}
	char zero = 0;
	lt_buffer_push(vm, &out, &zero);
	return out.data;
}

void ltasync_init_state(lt_VM* vm)
{
	vm->microtasks = lt_buffer_new(sizeof(lt_Microtask));
	vm->async_calls = lt_buffer_new(sizeof(lt_AsyncCall));
	vm->timers = lt_buffer_new(sizeof(lt_Timer));
	vm->workers = lt_buffer_new(sizeof(lt_Worker*));
	vm->next_timer_id = 1;
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
	if (worker->error) free(worker->error);
	if (worker->result_literal) free(worker->result_literal);
	vm->free(worker->state_literal);
	vm->free(worker->source);
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
	}
}

uint8_t lt_poll(lt_VM* vm)
{
	uint8_t did_work = 0;
	uint8_t has_pending = 0;
	uint8_t has_next_timer = 0;
	uint64_t next_timer_due = 0;

	if (vm->async_calls.length > 0)
	{
		lt_AsyncCall task = *(lt_AsyncCall*)lt_buffer_at(&vm->async_calls, 0);
		lt_buffer_cycle(&vm->async_calls, 0);

		for (uint8_t i = 0; i < task.argc; ++i) lt_push(vm, task.args[i]);
		uint16_t nret = lt_exec(vm, task.callee, task.argc);
		lt_Value result = LT_VALUE_NULL;
		if (nret > 0)
		{
			result = lt_pop(vm);
			for (uint16_t i = 1; i < nret; ++i) lt_pop(vm);
		}

		_lt_settle_promise(vm, task.promise, LT_PROMISE_FULFILLED, result);
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
			_lt_settle_promise(vm, promise, LT_PROMISE_FULFILLED, _lt_eval_literal(vm, worker->result_literal ? worker->result_literal : "null"));

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
	while (lt_poll(vm)) {}
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
	vm->top -= argc;

	lt_buffer_push(vm, &vm->async_calls, &task);
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

#if defined(_WIN32)
static DWORD WINAPI _lt_worker_main(LPVOID arg)
#else
static void* _lt_worker_main(void* arg)
#endif
{
	lt_Worker* worker = (lt_Worker*)arg;

	lt_VM* vm = lt_open(malloc, free, _lt_worker_error);
	if (!vm)
	{
		_lt_worker_set_error(worker, "Failed to create worker VM!");
		_lt_worker_mark_done(worker);
#if defined(_WIN32)
		return 0;
#else
		return 0;
#endif
	}
	vm->error_context = worker;
	ltstd_open_all(vm);

	uint32_t len = (uint32_t)(strlen(worker->state_literal) + strlen(worker->source) + 32);
	char* source = malloc(len);
	if (!source)
	{
		_lt_worker_set_error(worker, "Failed to allocate worker source!");
		lt_destroy(vm);
		_lt_worker_mark_done(worker);
#if defined(_WIN32)
		return 0;
#else
		return 0;
#endif
	}
	snprintf(source, len, "var state = %s\n%s", worker->state_literal, worker->source);

	uint32_t nret = lt_dostring(vm, source, "worker");
	free(source);

	_lt_worker_lock(worker);
	uint8_t has_error = worker->error != 0;
	_lt_worker_unlock(worker);

	if (!has_error)
	{
		lt_Value value = nret > 0 ? lt_pop(vm) : LT_VALUE_NULL;
		char error[128] = { 0 };
		char* result_literal = _lt_serialize_to_string(vm, value, error, 128);
		_lt_worker_lock(worker);
		worker->result_literal = result_literal;
		if (!worker->result_literal && !worker->error)
		{
			_lt_worker_unlock(worker);
			_lt_worker_set_error(worker, error);
			_lt_worker_lock(worker);
		}
		_lt_worker_unlock(worker);
	}

	lt_destroy(vm);
	_lt_worker_mark_done(worker);
#if defined(_WIN32)
	return 0;
#else
	return 0;
#endif
}

static lt_Value _lt_eval_literal(lt_VM* vm, const char* literal)
{
	uint32_t len = (uint32_t)strlen(literal) + 16;
	char* source = vm->alloc(len);
	snprintf(source, len, "return %s", literal);
	uint32_t nret = lt_dostring(vm, source, "worker-result");
	vm->free(source);
	if (nret == 0) return LT_VALUE_NULL;
	lt_Value result = lt_pop(vm);
	for (uint32_t i = 1; i < nret; ++i) lt_pop(vm);
	return result;
}

uint8_t ltasync_native_task_run(lt_VM* vm, uint8_t argc)
{
	if (argc != 2) lt_runtime_error(vm, "Expected source and state for task.run!");
	lt_Value state = lt_pop(vm);
	lt_Value source_value = lt_pop(vm);
	if (!LT_IS_STRING(source_value)) lt_runtime_error(vm, "Expected task.run source to be a string!");

	lt_Value promise = _lt_make_promise(vm);
	lt_nocollect(vm, LT_GET_OBJECT(promise));

	char error[128] = { 0 };
	char* state_literal = _lt_serialize_to_string(vm, state, error, 128);
	if (!state_literal)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, error));
		lt_push(vm, promise);
		lt_resumecollect(vm, LT_GET_OBJECT(promise));
		return 1;
	}

	lt_Worker* worker = vm->alloc(sizeof(lt_Worker));
	memset(worker, 0, sizeof(lt_Worker));
	_lt_worker_init_lock(worker);
	worker->parent = vm;
	worker->promise = LT_GET_OBJECT(promise);
	worker->state_literal = state_literal;
	worker->state = state;

	const char* source = lt_get_string(vm, source_value);
	uint32_t source_len = (uint32_t)strlen(source);
	worker->source = vm->alloc(source_len + 1);
	memcpy(worker->source, source, source_len + 1);

#if defined(_WIN32)
	worker->handle = CreateThread(0, 0, _lt_worker_main, worker, 0, 0);
	if (!worker->handle)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker thread!"));
		vm->free(worker->source);
		vm->free(worker->state_literal);
		_lt_worker_destroy_lock(worker);
		vm->free(worker);
	}
	else lt_buffer_push(vm, &vm->workers, &worker);
#else
	if (pthread_create(&worker->thread, 0, _lt_worker_main, worker) != 0)
	{
		_lt_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create worker thread!"));
		vm->free(worker->source);
		vm->free(worker->state_literal);
		_lt_worker_destroy_lock(worker);
		vm->free(worker);
	}
	else lt_buffer_push(vm, &vm->workers, &worker);
#endif

	lt_push(vm, promise);
	lt_resumecollect(vm, LT_GET_OBJECT(promise));
	return 1;
}

void ltasync_open_all(lt_VM* vm)
{
	ltasync_open_promise(vm);
	ltasync_open_timer(vm);
	ltasync_open_task(vm);
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
