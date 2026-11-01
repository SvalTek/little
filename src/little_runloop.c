#include "little_async_internal.h"
#include "little_buffer_internal.h"

uint32_t lt_add_poll_hook(lt_VM* vm, lt_PollHook hook, void* context)
{
	return ltasync_add_owned_poll_hook(vm, hook, context, 0);
}

void lt_remove_poll_hook(lt_VM* vm, uint32_t hook_id)
{
	if (hook_id == 0) return;
	for (uint32_t i = 0; i < vm->poll_hooks.length; ++i)
	{
		lt_PollHookEntry* entry = lt_buffer_at(&vm->poll_hooks, i);
		if (entry->id == hook_id)
		{
			entry->removed = 1;
			return;
		}
	}
}

uint8_t lt_poll(lt_VM* vm)
{
	return ltasync_poll_internal(vm, 1);
}

uint8_t lt_poll_now(lt_VM* vm)
{
	return ltasync_poll_internal(vm, 0);
}

void lt_runloop(lt_VM* vm)
{
	while (!vm->runloop_stop && lt_poll(vm)) {}
}
