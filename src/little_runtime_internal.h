#pragma once

#include "little.h"

uint8_t lt_native_is_promise_internal(lt_Value value);
lt_PromiseState lt_native_promise_state_internal(lt_Value value);
lt_Value lt_native_promise_result_internal(lt_Value value);
void lt_native_root_internal(lt_VM* vm, lt_Value value);
void lt_native_root_persistent_internal(lt_VM* vm, lt_Value value);
void lt_native_unroot_persistent_internal(lt_VM* vm, lt_Value value);

void lt_table_init_internal(lt_Table* table);
void lt_table_destroy_internal(lt_VM* vm, lt_Table* table);
void lt_table_mark_internal(lt_VM* vm, lt_Table* table);
lt_Value lt_table_get_raw_internal(lt_Table* table, lt_Value key);
lt_Value lt_table_set_raw_internal(lt_VM* vm, lt_Table* table, lt_Value key, lt_Value val);
lt_TablePair* lt_table_index_raw_internal(lt_VM* vm, lt_Table* table, lt_Value key, uint8_t alloc);
lt_TablePair* lt_table_index_internal(lt_VM* vm, lt_Value table, lt_Value key, uint8_t alloc);
void lt_name_value_internal(lt_VM* vm, lt_Value parent_name, lt_Value key, lt_Value val);
lt_Object* lt_native_callee_internal(lt_Object* callee);

lt_Value lt_class_call_internal(lt_VM* vm, lt_Value class_value, uint8_t argc);
lt_Value lt_make_class_internal(lt_VM* vm, lt_Value name);
lt_Value lt_instance_get_internal(lt_VM* vm, lt_Value instance_value, lt_Value key);
void lt_instance_set_internal(lt_VM* vm, lt_Value instance_value, lt_Value key, lt_Value value);
void lt_class_set_member_internal(lt_VM* vm, lt_Value class_value, lt_Value key, lt_Value value, int16_t encoded);
void lt_class_set_superclass_internal(lt_VM* vm, lt_Value class_value, lt_Value superclass_value);
lt_Value lt_find_class_constructor_internal(lt_Object* klass);

void lt_sweep_v(lt_VM* vm, lt_Value val);
uint16_t lt_exec_internal(lt_VM* vm, lt_Value callable, uint8_t argc);
