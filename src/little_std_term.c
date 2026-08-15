#include "little_std.h"
#include "little_loadlib.h"

lt_Value ltopen(lt_VM* vm, const lt_Api* api);
void lt_term_shutdown(void);

void ltstd_open_term(lt_VM* vm)
{
    lt_Value term = ltopen(vm, ltstd_native_api());
    if (LT_IS_NULL(term)) lt_runtime_error(vm, "Built-in terminal module rejected the Little API!");
    lt_table_set(vm, vm->global, lt_make_string(vm, "term"), term);
}

void ltstd_close_term(void)
{
    lt_term_shutdown();
}
