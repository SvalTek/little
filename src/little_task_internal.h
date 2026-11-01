#pragma once

#include "little_async_internal.h"

uint8_t ltasync_native_task_run(lt_VM* vm, uint8_t argc);

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

void ltasync_worker_lock_internal(lt_Worker* worker);
void ltasync_worker_unlock_internal(lt_Worker* worker);
void ltasync_worker_init_lock_internal(lt_Worker* worker);
void ltasync_worker_destroy_lock_internal(lt_Worker* worker);
void ltasync_worker_mark_done_internal(lt_Worker* worker);
void ltasync_worker_set_error_internal(lt_Worker* worker, const char* msg);
void ltasync_keep_value_internal(lt_VM* vm, lt_Value value);
void ltasync_release_value_internal(lt_VM* vm, lt_Value value);
