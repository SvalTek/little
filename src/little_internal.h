#ifndef LITTLE_INTERNAL_H
#define LITTLE_INTERNAL_H

#include "little.h"

uint8_t lt_buffer_push(lt_VM* vm, lt_Buffer* buf, void* element);
void* lt_buffer_at(lt_Buffer* buf, uint32_t idx);
void* lt_buffer_last(lt_Buffer* buf);
void lt_buffer_cycle(lt_Buffer* buf, uint32_t idx);
void lt_buffer_pop(lt_Buffer* buf);

void lt_sweep_v(lt_VM* vm, lt_Value val);
uint16_t _lt_exec(lt_VM* vm, lt_Value callable, uint8_t argc);

void ltasync_init_state(lt_VM* vm);
void ltasync_destroy_state(lt_VM* vm);
void ltasync_mark_roots(lt_VM* vm);
void ltasync_mark_promise(lt_VM* vm, lt_Object* promise);
void ltasync_free_promise(lt_VM* vm, lt_Object* promise);
lt_Value ltasync_get_promise_method(lt_VM* vm, lt_Value promise, lt_Value key);
uint8_t ltasync_is_async_callable(lt_Value callable);
lt_Value ltasync_call(lt_VM* vm, lt_Value callee, uint8_t argc);
lt_Value ltasync_await(lt_VM* vm, lt_Value value);

#endif
