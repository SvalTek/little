#pragma once

#include "little.h"

typedef struct {
	uint32_t id;
	lt_PollHook hook;
	void* context;
	uint8_t removed;
	void (*destroy)(lt_VM*, void*);
} lt_PollHookEntry;

uint8_t ltasync_poll_internal(lt_VM* vm, int allow_sleep);
