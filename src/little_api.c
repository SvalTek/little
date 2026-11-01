#include "little_buffer_internal.h"
#include "little_runtime_internal.h"

static void* _lt_api_alloc(lt_VM* vm, size_t size)
{
    return vm->alloc(size);
}

static void _lt_api_free(lt_VM* vm, void* ptr)
{
    vm->free(ptr);
}

uint8_t lt_native_is_promise_internal(lt_Value value)
{
    return LT_IS_OBJECT(value) && LT_GET_OBJECT(value)->type == LT_OBJECT_PROMISE;
}

lt_PromiseState lt_native_promise_state_internal(lt_Value value)
{
    if (!lt_native_is_promise_internal(value)) return LT_PROMISE_REJECTED;
    return LT_GET_OBJECT(value)->promise.state;
}

lt_Value lt_native_promise_result_internal(lt_Value value)
{
    if (!lt_native_is_promise_internal(value)) return LT_VALUE_NULL;
    return LT_GET_OBJECT(value)->promise.result;
}

void lt_native_root_internal(lt_VM* vm, lt_Value value)
{
    lt_buffer_push(vm, &vm->native_roots, &value);
}

static void _lt_api_unroot(lt_VM* vm, lt_Value value)
{
    for (uint32_t i = vm->native_root_floor; i < vm->native_roots.length; ++i)
    {
        if (*(lt_Value*)lt_buffer_at(&vm->native_roots, i) == value)
        {
            lt_buffer_cycle(&vm->native_roots, i);
            return;
        }
    }
}

void lt_native_root_persistent_internal(lt_VM* vm, lt_Value value)
{
    if (LT_IS_OBJECT(value)) lt_nocollect(vm, LT_GET_OBJECT(value));
}

void lt_native_unroot_persistent_internal(lt_VM* vm, lt_Value value)
{
    if (LT_IS_OBJECT(value)) lt_resumecollect(vm, LT_GET_OBJECT(value));
}

static const lt_Api _lt_native_api = {
    .version = LT_API_VERSION,
    .size = sizeof(lt_Api),
    .alloc = _lt_api_alloc,
    .free = _lt_api_free,
    .runtime_error = lt_runtime_error,
    .push = lt_push,
    .pop = lt_pop,
    .exec = lt_exec,
    .make_number = lt_make_number,
    .get_number = lt_get_number,
    .make_string = lt_make_string,
    .get_string = lt_get_string,
    .make_table = lt_make_table,
    .make_array = lt_make_array,
    .make_native = lt_make_native,
    .make_ptr = lt_make_ptr,
    .get_ptr = lt_get_ptr,
    .table_set = lt_table_set,
    .table_get = lt_table_get,
    .table_next = lt_table_next,
    .table_pop = lt_table_pop,
    .array_push = lt_array_push,
    .array_get = lt_array_get,
    .array_set = lt_array_set,
    .array_remove = lt_array_remove,
    .array_length = lt_array_length,
    .poll = lt_poll,
    .add_poll_hook = lt_add_poll_hook,
    .remove_poll_hook = lt_remove_poll_hook,
    .is_promise = lt_native_is_promise_internal,
    .promise_state = lt_native_promise_state_internal,
    .promise_result = lt_native_promise_result_internal,
    .poll_now = lt_poll_now,
    .root = lt_native_root_internal,
    .unroot = _lt_api_unroot,
    .equals = lt_equals,
    .class_create = lt_class_create,
    .class_set_constructor = lt_class_set_constructor,
    .class_add_method = lt_class_add_method,
    .class_add_getter = lt_class_add_getter,
    .class_add_setter = lt_class_add_setter,
    .class_set_native_data_destroy = lt_class_set_native_data_destroy,
    .instance_set_native_data = lt_instance_set_native_data,
    .instance_get_native_data = lt_instance_get_native_data,
    .instance_dispose_native_data = lt_instance_dispose_native_data,
    .instance_clear_native_data = lt_instance_clear_native_data,
    .root_persistent = lt_native_root_persistent_internal,
    .unroot_persistent = lt_native_unroot_persistent_internal,
};

const lt_Api* ltstd_native_api(void)
{
    return &_lt_native_api;
}
