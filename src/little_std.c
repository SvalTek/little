#include "little_std.h"
#include "little_std_internal.h"
#include "little.h"
#include "little_runtime_internal.h"
#include "little_common.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t _ltstd_is_callable(lt_Value value)
{
    if (!LT_IS_OBJECT(value)) return 0;
    lt_Object* obj = LT_GET_OBJECT(value);
    return obj->type == LT_OBJECT_FN
        || obj->type == LT_OBJECT_CLOSURE
        || obj->type == LT_OBJECT_NATIVEFN
        || obj->type == LT_OBJECT_BOUND_NATIVE
        || obj->type == LT_OBJECT_CLASS;
}

static lt_Value _ltstd_pcall_result(lt_VM* vm, uint8_t ok, lt_Value value, const char* error)
{
    lt_Value result = lt_make_table(vm);
    lt_table_set(vm, result, lt_make_string(vm, "ok"), ok ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    if (ok) lt_table_set(vm, result, lt_make_string(vm, "value"), value);
    else lt_table_set(vm, result, lt_make_string(vm, "error"), lt_make_string(vm, error ? error : "Unknown error"));
    return result;
}

static uint8_t _ltstd_pcall(lt_VM* vm, uint8_t argc)
{
    if (argc < 1) lt_runtime_error(vm, "Expected callable argument to pcall!");

    uint16_t base = vm->top - argc;
    lt_Value callable = vm->stack[base];
    if (!_ltstd_is_callable(callable)) lt_runtime_error(vm, "Expected callable argument to pcall!");

    for (uint16_t i = base + 1; i < vm->top; ++i)
        vm->stack[i - 1] = vm->stack[i];
    vm->top--;

    uint16_t saved_depth = vm->depth;
    lt_Frame* saved_current = vm->current;
    uint32_t saved_native_roots = vm->native_roots.length;
    uint32_t saved_temporary_roots = vm->temporary_roots.length;
    uint32_t saved_native_root_floor = vm->native_root_floor;
    void* saved_error_buf = vm->error_buf;
    uint8_t saved_trap_errors = vm->trap_errors;
    char* saved_error_trap = vm->error_trap;

    jmp_buf error_buf;
    vm->error_buf = &error_buf;
    vm->trap_errors = 1;
    vm->error_trap = 0;

    if (!setjmp(error_buf))
    {
        uint16_t nret = lt_exec_internal(vm, callable, argc - 1);
        lt_Value value = LT_VALUE_NULL;
        if (nret > 0)
        {
            value = lt_pop(vm);
            while (--nret > 0) lt_pop(vm);
        }

        vm->error_buf = saved_error_buf;
        vm->trap_errors = saved_trap_errors;
        if (vm->error_trap) vm->free(vm->error_trap);
        vm->error_trap = saved_error_trap;

        lt_push(vm, _ltstd_pcall_result(vm, 1, value, 0));
        return 1;
    }

    char* error = vm->error_trap;
    vm->top = base;
    vm->depth = saved_depth;
    vm->current = saved_current;
    vm->native_roots.length = saved_native_roots;
    vm->temporary_roots.length = saved_temporary_roots;
    vm->native_root_floor = saved_native_root_floor;
    vm->error_buf = saved_error_buf;
    vm->trap_errors = saved_trap_errors;
    vm->error_trap = saved_error_trap;

    lt_push(vm, _ltstd_pcall_result(vm, 0, LT_VALUE_NULL, error));
    if (error) vm->free(error);
    return 1;
}

static uint8_t _ltstd_unpack(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 3) lt_runtime_error(vm, "Expected 1-3 arguments to unpack!");

    lt_Value end_value = LT_VALUE_NULL;
    lt_Value start_value = LT_VALUE_NUMBER(0);
    if (argc == 3) end_value = lt_pop(vm);
    if (argc >= 2) start_value = lt_pop(vm);
    lt_Value array = lt_pop(vm);

    if (!LT_IS_ARRAY(array)) lt_runtime_error(vm, "Expected first argument to unpack to be array!");
    if (!LT_IS_NUMBER(start_value)) lt_runtime_error(vm, "Expected start argument to unpack to be number!");
    if (!LT_IS_NULL(end_value) && !LT_IS_NUMBER(end_value)) lt_runtime_error(vm, "Expected end argument to unpack to be number!");

    int32_t start = (int32_t)LT_GET_NUMBER(start_value);
    int32_t end = LT_IS_NULL(end_value) ? (int32_t)lt_array_length(array) - 1 : (int32_t)LT_GET_NUMBER(end_value);
    if (start < 0 || end < -1) lt_runtime_error(vm, "Expected unpack bounds to be non-negative!");
    if (start > end || start >= (int32_t)lt_array_length(array)) return 0;
    if (end >= (int32_t)lt_array_length(array)) end = (int32_t)lt_array_length(array) - 1;

    uint8_t count = 0;
    for (int32_t i = start; i <= end && count < LT_MAX_RETURNS; ++i)
    {
        lt_push(vm, lt_array_get(vm, array, (uint32_t)i));
        count++;
    }
    return count;
}

void ltstd_open_all(lt_VM* vm)
{
    /* Worker VMs spawned by tasks inherit the standard library when the host
       installed it, without the core library depending on this module. */
    lt_set_worker_vm_init(vm, ltstd_open_all);

    /** [LittleBinding] pcall
     *  calls a callable with the remaining arguments and returns { ok: true value: ... } or { ok: false error: ... } instead of letting runtime errors escape.
     *  @signature (fn [, args...])
     *  @param fn callable to invoke
     *  @param args... arguments passed to fn
     *  @return table
     */
    lt_table_set(vm, vm->global, lt_make_string(vm, "pcall"), lt_make_native(vm, _ltstd_pcall));
    /** [LittleBinding] unpack
     *  returns the elements of an array from start through end as positional return values.
     *  @signature (array [, start [, end]])
     *  @param array array to read elements from
     *  @param start first index to read, defaults to 0
     *  @param end last index to read, inclusive, defaults to the last index
     */
    lt_table_set(vm, vm->global, lt_make_string(vm, "unpack"), lt_make_native(vm, _ltstd_unpack));
    /** [LittleBinding] import
     *  loads the module at path from the module cache or from disk and returns its exported value.
     *  @signature (path)
     *  @param path module path to load
     */
    lt_table_set(vm, vm->global, lt_make_string(vm, "import"), lt_make_native(vm, ltstd_import_internal));
    lt_Value module = lt_make_table(vm);
    /** [LittleBinding] module.addPath
     *  appends one or more directory search paths used to resolve imported modules.
     *  @signature (path [, ...])
     *  @param path search path to append; further paths may follow
     *  @return null
     */
    lt_table_set(vm, module, lt_make_string(vm, "addPath"), lt_make_native(vm, ltstd_module_add_path_internal));
    /** [LittleBinding] module.clearPaths
     *  removes every registered module search path.
     *  @signature ()
     *  @return null
     */
    lt_table_set(vm, module, lt_make_string(vm, "clearPaths"), lt_make_native(vm, ltstd_module_clear_paths_internal));
    lt_table_set(vm, vm->global, lt_make_string(vm, "module"), module);
    lt_add_module_loader(vm, ltstd_file_module_loader_internal, 0);
    ltstd_open_io(vm);
    ltstd_open_math(vm);
    ltstd_open_number(vm);
    ltstd_open_bit(vm);
    ltstd_open_array(vm);
    ltstd_open_table(vm);
    ltstd_open_string(vm);
    ltstd_open_gc(vm);
    ltstd_open_json(vm);
    ltstd_open_web(vm);
    ltstd_open_ws(vm);
}
