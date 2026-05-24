#ifndef LITTLE_INTERNAL_H
#define LITTLE_INTERNAL_H

#include "little.h"

uint8_t lt_buffer_push(lt_VM* vm, lt_Buffer* buf, void* element);
void* lt_buffer_at(lt_Buffer* buf, uint32_t idx);
void* lt_buffer_last(lt_Buffer* buf);
void lt_buffer_cycle(lt_Buffer* buf, uint32_t idx);
void lt_buffer_pop(lt_Buffer* buf);

void lt_sweep_v(lt_VM* vm, lt_Value val);
uint16_t lt_exec_internal(lt_VM* vm, lt_Value callable, uint8_t argc);

void ltasync_init_state(lt_VM* vm);
void ltasync_destroy_state(lt_VM* vm);
void ltasync_mark_roots(lt_VM* vm);
void ltasync_mark_promise(lt_VM* vm, lt_Object* promise);
void ltasync_free_promise(lt_VM* vm, lt_Object* promise);
lt_Value ltasync_get_promise_method(lt_VM* vm, lt_Value promise, lt_Value key);
uint8_t ltasync_is_async_callable(lt_Value callable);
lt_Value ltasync_call(lt_VM* vm, lt_Value callee, uint8_t argc);
lt_Value ltasync_await(lt_VM* vm, lt_Value value);

void ltshared_retain(lt_SharedObject* shared);
void ltshared_release(lt_SharedObject* shared);
lt_Value ltshared_make_proxy(lt_VM* vm, lt_SharedObject* shared);
lt_Value ltshared_table_get(lt_VM* vm, lt_SharedObject* shared, lt_Value key);
lt_Value ltshared_table_set(lt_VM* vm, lt_SharedObject* shared, lt_Value key, lt_Value val);
lt_Value ltshared_table_keys(lt_VM* vm, lt_SharedObject* shared);
lt_Value ltshared_table_values(lt_VM* vm, lt_SharedObject* shared);
lt_Value ltshared_array_get(lt_VM* vm, lt_SharedObject* shared, uint32_t idx);
lt_Value ltshared_array_set(lt_VM* vm, lt_SharedObject* shared, uint32_t idx, lt_Value val);
lt_Value ltshared_array_push(lt_VM* vm, lt_SharedObject* shared, lt_Value val);
lt_Value ltshared_array_remove(lt_VM* vm, lt_SharedObject* shared, uint32_t idx);
uint32_t ltshared_array_length(lt_SharedObject* shared);

#endif
