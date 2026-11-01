#include "little_async.h"
#include "little_task_internal.h"
#include "little_buffer_internal.h"

static uint8_t _lt_promise_resolve_native(lt_VM* vm, uint8_t argc);
static uint8_t _lt_promise_reject_native(lt_VM* vm, uint8_t argc);

static uint8_t _lt_add_timer(lt_VM* vm, uint8_t argc, uint8_t repeat)
{
	if (argc != 2) lt_runtime_error(vm, repeat ? "Expected callback and delay for setInterval!" : "Expected callback and delay for setTimeout!");
	lt_Value delay = lt_pop(vm);
	lt_Value callback = lt_pop(vm);
	if (!LT_IS_NUMBER(delay)) lt_runtime_error(vm, "Expected timer delay to be a number!");
	if (!ltasync_is_callable(callback)) lt_runtime_error(vm, "Expected timer callback to be callable!");

	double delay_number = lt_get_number(delay);
	if (delay_number < 0) lt_runtime_error(vm, "Expected timer delay to be non-negative!");
	uint64_t delay_ms = (uint64_t)delay_number;
	lt_Timer timer;
	timer.id = vm->next_timer_id++;
	timer.due_ms = ltasync_now_ms_internal() + delay_ms;
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
	if (!ltasync_is_callable(executor)) lt_runtime_error(vm, "Expected Promise executor to be callable!");

	lt_Value promise = ltasync_make_promise(vm);
	lt_nocollect(vm, LT_GET_OBJECT(promise));
	lt_Value resolve = ltasync_make_bound_native_internal(vm, _lt_promise_resolve_native, promise);
	lt_Value reject = ltasync_make_bound_native_internal(vm, _lt_promise_reject_native, promise);

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
	lt_Value promise = ltasync_current_receiver_internal(vm);
	lt_Value value = argc > 0 ? lt_pop(vm) : LT_VALUE_NULL;
	while (argc-- > 1) lt_pop(vm);
	ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, value);
	return 0;
}

static uint8_t _lt_promise_reject_native(lt_VM* vm, uint8_t argc)
{
	lt_Value promise = ltasync_current_receiver_internal(vm);
	lt_Value value = argc > 0 ? lt_pop(vm) : LT_VALUE_NULL;
	while (argc-- > 1) lt_pop(vm);
	ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, value);
	return 0;
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

void ltasync_open_all(lt_VM* vm)
{
	ltasync_open_promise(vm);
	ltasync_open_timer(vm);
	ltasync_open_task(vm);
	ltasync_open_mainloop(vm);
}

void ltasync_open_promise(lt_VM* vm)
{
	/** [LittleBinding] Promise
	 *  creates a promise and calls executor immediately with resolve and reject callbacks.
	 *  @signature (executor)
	 *  @param executor function called with resolve and reject callbacks
	 */
	lt_table_set(vm, vm->global, lt_make_string(vm, "Promise"), lt_make_native(vm, ltasync_native_promise));
}

void ltasync_open_timer(lt_VM* vm)
{
	/** [LittleBinding] setTimeout
	 *  runs callback once after ms milliseconds and returns a timer id.
	 *  @signature (callback, ms)
	 *  @param callback function invoked once after the delay
	 *  @param ms delay in milliseconds
	 *  @return number
	 */
	lt_table_set(vm, vm->global, lt_make_string(vm, "setTimeout"), lt_make_native(vm, ltasync_native_set_timeout));
	/** [LittleBinding] setInterval
	 *  runs callback every ms milliseconds until the timer is cancelled and returns a timer id.
	 *  @signature (callback, ms)
	 *  @param callback function invoked on each tick
	 *  @param ms interval in milliseconds
	 *  @return number
	 */
	lt_table_set(vm, vm->global, lt_make_string(vm, "setInterval"), lt_make_native(vm, ltasync_native_set_interval));
	/** [LittleBinding] clearTimeout
	 *  cancels the timer with the given id.
	 *  @signature (id)
	 *  @param id timer id returned by setTimeout or setInterval
	 *  @return null
	 */
	lt_table_set(vm, vm->global, lt_make_string(vm, "clearTimeout"), lt_make_native(vm, ltasync_native_clear_timer));
	/** [LittleBinding] clearInterval
	 *  cancels the timer with the given id.
	 *  @signature (id)
	 *  @param id timer id returned by setInterval or setTimeout
	 *  @return null
	 */
	lt_table_set(vm, vm->global, lt_make_string(vm, "clearInterval"), lt_make_native(vm, ltasync_native_clear_timer));
}

void ltasync_open_task(lt_VM* vm)
{
	lt_Value task = lt_make_table(vm);
	/** [LittleBinding] task.run
	 *  runs a Little function on a worker thread with a marshalled state value and returns a promise that resolves to its result or rejects with an error string.
	 *  @signature (callable, state)
	 *  @param callable Little function to run in the worker
	 *  @param state value copied into the worker and passed to callable
	 */
	lt_table_set(vm, task, lt_make_string(vm, "run"), lt_make_native(vm, ltasync_native_task_run));
	lt_table_set(vm, vm->global, lt_make_string(vm, "task"), task);
}

void ltasync_open_mainloop(lt_VM* vm)
{
	lt_Value mainloop = lt_make_table(vm);
	/** [LittleBinding] mainloop.run
	 *  drains pending async work until the VM is idle or mainloop.stop is called and returns the number of poll steps performed.
	 *  @signature ()
	 *  @return number
	 */
	lt_table_set(vm, mainloop, lt_make_string(vm, "run"), lt_make_native(vm, ltasync_native_mainloop_run));
	/** [LittleBinding] mainloop.poll
	 *  advances one async step and returns whether work was performed or remains pending.
	 *  @signature ()
	 *  @return bool
	 */
	lt_table_set(vm, mainloop, lt_make_string(vm, "poll"), lt_make_native(vm, ltasync_native_mainloop_poll));
	/** [LittleBinding] mainloop.stop
	 *  requests that the current script-managed mainloop stop.
	 *  @signature ()
	 *  @return null
	 */
	lt_table_set(vm, mainloop, lt_make_string(vm, "stop"), lt_make_native(vm, ltasync_native_mainloop_stop));
	lt_table_set(vm, vm->global, lt_make_string(vm, "mainloop"), mainloop);
}
