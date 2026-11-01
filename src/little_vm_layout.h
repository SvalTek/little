#pragma once

/* Advanced compatibility interface for code that accesses VM internals. */
#include "little_api.h"

typedef struct lt_Frame {
	lt_Object* callee;
	lt_Buffer* code;
	lt_Buffer* constants;
	lt_Buffer* upvals;
	lt_Object* class_context;
	uint32_t pc;
	uint16_t start;
} lt_Frame;

struct lt_VM {
	lt_Buffer heap;
	lt_Buffer keepalive;
	lt_Buffer native_roots;
	uint32_t native_root_floor;

	uint16_t top;
	lt_Value stack[LT_STACK_SIZE];

	uint16_t depth;
	lt_Frame callstack[LT_CALLSTACK_SIZE];
	lt_Frame* current;

	lt_Buffer strings[LT_DEDUP_TABLE_SIZE];

	lt_Value global;
	lt_Buffer microtasks;
	lt_Buffer async_calls;
	lt_Buffer timers;
	lt_Buffer workers;
	lt_Buffer poll_hooks;
	lt_Buffer native_libraries;
	lt_Buffer module_loaders;
	uint32_t next_timer_id;
	uint32_t next_poll_hook_id;
	uint8_t runloop_stop;
	uint8_t last_call_returns;

	lt_AllocFn alloc;
	lt_FreeFn free;
	lt_ErrorFn error;

	void* error_buf;
	void* error_context;
	char* error_trap;
	uint8_t trap_errors;
	uint8_t generate_debug;

	/* Appended to preserve offsets of fields exposed by earlier headers. */
	lt_Buffer temporary_roots;
	uint64_t rng_state;
	uint64_t rng_inc;
	uint8_t rng_seeded;
	lt_VMInitFn worker_vm_init;
	uint8_t collect_diagnostics;
	uint16_t error_line;
	lt_Buffer diagnostics;
	uint16_t error_col;
	uint8_t error_at_token;
};
