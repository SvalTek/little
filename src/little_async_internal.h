#pragma once

#include "little.h"
#include "little_shared_internal.h"
#include "little_runloop_internal.h"

typedef struct {
	uint32_t id;
	uint64_t due_ms;
	uint64_t interval_ms;
	lt_Value callback;
	uint8_t repeat;
	uint8_t cancelled;
} lt_Timer;

uint64_t ltasync_now_ms_internal(void);
lt_Value ltasync_make_bound_native_internal(lt_VM* vm, lt_NativeFn fn, lt_Value receiver);
lt_Value ltasync_current_receiver_internal(lt_VM* vm);

uint8_t ltasync_native_promise(lt_VM* vm, uint8_t argc);
uint8_t ltasync_native_set_timeout(lt_VM* vm, uint8_t argc);
uint8_t ltasync_native_set_interval(lt_VM* vm, uint8_t argc);
uint8_t ltasync_native_clear_timer(lt_VM* vm, uint8_t argc);
void ltasync_init_state(lt_VM* vm);
void ltasync_destroy_state(lt_VM* vm);
void ltasync_mark_roots(lt_VM* vm);
void ltasync_mark_promise(lt_VM* vm, lt_Object* promise);
void ltasync_free_promise(lt_VM* vm, lt_Object* promise);
lt_Value ltasync_get_promise_method(lt_VM* vm, lt_Value promise, lt_Value key);
uint8_t ltasync_is_async_callable(lt_Value callable);
uint32_t ltasync_add_owned_poll_hook(lt_VM* vm, lt_PollHook hook, void* context, void (*destroy)(lt_VM*, void*));
uint8_t ltasync_is_callable(lt_Value value);
lt_Value ltasync_call(lt_VM* vm, lt_Value callee, uint8_t argc);
lt_Value ltasync_await(lt_VM* vm, lt_Value value);
lt_Value ltasync_make_promise(lt_VM* vm);
void ltasync_settle_promise(lt_VM* vm, lt_Value promise, lt_PromiseState state, lt_Value result);
