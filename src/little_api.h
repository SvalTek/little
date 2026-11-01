#pragma once

#include "little_value.h"

typedef enum {
	LT_POLL_IDLE,
	LT_POLL_WORK,
	LT_POLL_PENDING,
} lt_PollResult;

typedef lt_PollResult(*lt_PollHook)(lt_VM* vm, void* context);

typedef void* (*lt_AllocFn)(size_t);
typedef void (*lt_FreeFn)(void*);
typedef void (*lt_ErrorFn)(lt_VM* vm, const char*);
/* Installs host libraries into a VM. Used for worker VMs spawned by tasks so
   the core library does not have to link the standard library. */
typedef void (*lt_VMInitFn)(lt_VM* vm);
typedef void (*lt_NativeLibraryCloseFn)(void*);

typedef struct lt_NativeLibrary {
	void* handle;
	lt_NativeLibraryCloseFn close;
} lt_NativeLibrary;

typedef enum {
	LT_MODULE_LOADER_NOT_FOUND,
	LT_MODULE_LOADER_FOUND,
} lt_ModuleLoaderResult;

typedef lt_ModuleLoaderResult(*lt_ModuleLoaderFn)(lt_VM* vm, const char* requested, char** source, char** module_name, void* userdata);

typedef struct {
	lt_ModuleLoaderFn load;
	void* userdata;
} lt_ModuleLoader;

lt_VM* lt_open(lt_AllocFn alloc, lt_FreeFn free, lt_ErrorFn error);
void lt_destroy(lt_VM* vm);

lt_Buffer lt_buffer_new(uint32_t element_size);
void lt_buffer_destroy(lt_VM* vm, lt_Buffer* buf);

lt_Object* lt_allocate(lt_VM* vm, lt_ObjectType type);
void lt_free(lt_VM* vm, uint32_t heapidx);

void lt_nocollect(lt_VM* vm, lt_Object* obj);
void lt_resumecollect(lt_VM* vm, lt_Object* obj);
uint32_t lt_collect(lt_VM* vm);

void lt_push(lt_VM* vm, lt_Value val);
lt_Value lt_pop(lt_VM* vm);
lt_Value lt_at(lt_VM* vm, uint32_t idx);

void lt_close(lt_VM* vm, uint8_t count);
lt_Value lt_getupval(lt_VM* vm, uint8_t idx);
void lt_setupval(lt_VM* vm, uint8_t idx, lt_Value val);

uint16_t lt_exec(lt_VM* vm, lt_Value callable, uint8_t argc);
uint8_t lt_poll(lt_VM* vm);
uint8_t lt_poll_now(lt_VM* vm);
void lt_runloop(lt_VM* vm);
uint32_t lt_add_poll_hook(lt_VM* vm, lt_PollHook hook, void* context);
void lt_remove_poll_hook(lt_VM* vm, uint32_t hook_id);
void lt_add_module_loader(lt_VM* vm, lt_ModuleLoaderFn loader, void* userdata);
void lt_error(lt_VM* vm, const char* msg);
void lt_runtime_error(lt_VM* vm, const char* message);

void lt_set_worker_vm_init(lt_VM* vm, lt_VMInitFn init);

lt_Value lt_loadstring(lt_VM* vm, const char* source, const char* mod_name);
uint32_t lt_dostring(lt_VM* vm, const char* source, const char* mod_name);

lt_Value lt_make_number(double n);
double lt_get_number(lt_Value v);

lt_Value lt_make_string(lt_VM* vm, const char* string);
const char* lt_get_string(lt_VM* vm, lt_Value value);

uint8_t lt_equals(lt_Value a, lt_Value b);

lt_Value lt_make_table(lt_VM* vm);
lt_Value lt_table_set(lt_VM* vm, lt_Value table, lt_Value key, lt_Value val);
lt_Value lt_table_get(lt_VM* vm, lt_Value table, lt_Value key);
uint8_t  lt_table_next(lt_VM* vm, lt_Value table, uint64_t* cursor, lt_Value* key, lt_Value* val);
uint8_t  lt_table_pop(lt_VM* vm, lt_Value table, lt_Value key);

lt_Value  lt_make_array(lt_VM* vm);
lt_Value  lt_array_push(lt_VM* vm, lt_Value array, lt_Value val);
lt_Value  lt_array_get(lt_VM* vm, lt_Value array, uint32_t idx);
lt_Value  lt_array_set(lt_VM* vm, lt_Value array, uint32_t idx, lt_Value val);
lt_Value* lt_array_at(lt_Value array, uint32_t idx);
lt_Value  lt_array_remove(lt_VM* vm, lt_Value array, uint32_t idx);
uint32_t  lt_array_length(lt_Value array);

lt_Value lt_make_native(lt_VM* vm, lt_NativeFn fn);
lt_Value lt_make_ptr(lt_VM* vm, void* ptr);
void* lt_get_ptr(lt_Value ptr);

lt_Value lt_class_create(lt_VM* vm, const char* name);
void lt_class_set_constructor(lt_VM* vm, lt_Value klass, lt_NativeFn fn);
void lt_class_add_method(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override);
void lt_class_add_getter(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override);
void lt_class_add_setter(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override);
void lt_class_set_native_data_destroy(lt_VM* vm, lt_Value klass, lt_NativeDataDestroyFn destroy);
void lt_instance_set_native_data(lt_VM* vm, lt_Value instance, lt_Value native_class, void* data);
void* lt_instance_get_native_data(lt_VM* vm, lt_Value instance, lt_Value native_class);
void lt_instance_dispose_native_data(lt_VM* vm, lt_Value instance);
void lt_instance_clear_native_data(lt_VM* vm, lt_Value instance);

#define LT_API_VERSION 3

struct lt_Api {
	uint32_t version;
	uint32_t size;

	void* (*alloc)(lt_VM* vm, size_t size);
	void (*free)(lt_VM* vm, void* ptr);

	void (*runtime_error)(lt_VM* vm, const char* message);

	void (*push)(lt_VM* vm, lt_Value val);
	lt_Value (*pop)(lt_VM* vm);
	uint16_t (*exec)(lt_VM* vm, lt_Value callable, uint8_t argc);

	lt_Value (*make_number)(double n);
	double (*get_number)(lt_Value v);
	lt_Value (*make_string)(lt_VM* vm, const char* string);
	const char* (*get_string)(lt_VM* vm, lt_Value value);
	lt_Value (*make_table)(lt_VM* vm);
	lt_Value (*make_array)(lt_VM* vm);
	lt_Value (*make_native)(lt_VM* vm, lt_NativeFn fn);
	lt_Value (*make_ptr)(lt_VM* vm, void* ptr);
	void* (*get_ptr)(lt_Value ptr);

	lt_Value (*table_set)(lt_VM* vm, lt_Value table, lt_Value key, lt_Value val);
	lt_Value (*table_get)(lt_VM* vm, lt_Value table, lt_Value key);
	uint8_t (*table_next)(lt_VM* vm, lt_Value table, uint64_t* cursor, lt_Value* key, lt_Value* val);
	uint8_t (*table_pop)(lt_VM* vm, lt_Value table, lt_Value key);

	lt_Value (*array_push)(lt_VM* vm, lt_Value array, lt_Value val);
	lt_Value (*array_get)(lt_VM* vm, lt_Value array, uint32_t idx);
	lt_Value (*array_set)(lt_VM* vm, lt_Value array, uint32_t idx, lt_Value val);
	lt_Value (*array_remove)(lt_VM* vm, lt_Value array, uint32_t idx);
	uint32_t (*array_length)(lt_Value array);

	uint8_t (*poll)(lt_VM* vm);
	uint32_t (*add_poll_hook)(lt_VM* vm, lt_PollHook hook, void* context);
	void (*remove_poll_hook)(lt_VM* vm, uint32_t hook_id);
	uint8_t (*is_promise)(lt_Value value);
	lt_PromiseState (*promise_state)(lt_Value value);
	lt_Value (*promise_result)(lt_Value value);
	uint8_t (*poll_now)(lt_VM* vm);

	/* API v3 addendum: callback-scoped and persistent value roots. */
	void (*root)(lt_VM* vm, lt_Value value);
	void (*unroot)(lt_VM* vm, lt_Value value);
	uint8_t (*equals)(lt_Value a, lt_Value b);
	/* API v3 addendum: native-backed classes and instance payloads. */
	lt_Value (*class_create)(lt_VM* vm, const char* name);
	void (*class_set_constructor)(lt_VM* vm, lt_Value klass, lt_NativeFn fn);
	void (*class_add_method)(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override);
	void (*class_add_getter)(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override);
	void (*class_add_setter)(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override);
	void (*class_set_native_data_destroy)(lt_VM* vm, lt_Value klass, lt_NativeDataDestroyFn destroy);
	void (*instance_set_native_data)(lt_VM* vm, lt_Value instance, lt_Value native_class, void* data);
	void* (*instance_get_native_data)(lt_VM* vm, lt_Value instance, lt_Value native_class);
	void (*instance_dispose_native_data)(lt_VM* vm, lt_Value instance);
	void (*instance_clear_native_data)(lt_VM* vm, lt_Value instance);

	/* Roots retained beyond the current native callback, until explicitly removed. */
	void (*root_persistent)(lt_VM* vm, lt_Value value);
	void (*unroot_persistent)(lt_VM* vm, lt_Value value);
};

const lt_Api* ltstd_native_api(void);
