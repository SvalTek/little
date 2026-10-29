#include "little_std.h"
#include "little_internal.h"

/**
 * Opens the built-in JSON module and registers it as the global `json` module.
 * @param vm Virtual machine used to load and register the JSON module.
 */
void ltstd_open_json(lt_VM* vm)
{
    const lt_Api* api = ltstd_native_api();
    lt_json_init(api);
    lt_Value json = lt_json_open(vm, api);
    if (LT_IS_NULL(json)) lt_runtime_error(vm, "Built-in JSON module rejected the Little API!");
    lt_table_set(vm, vm->global, lt_make_string(vm, "json"), json);
}
