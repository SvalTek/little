#include "../../src/little.h"
#include "../../src/little_std.h"

#include <stdio.h>
#include <stdlib.h>

static void test_error(lt_VM* vm, const char* message)
{
    (void)vm;
    fprintf(stderr, "%s\n", message);
}

int main(void)
{
    lt_VM* vm = lt_open(malloc, free, test_error);
    if (!vm) return 1;

    ltstd_open_all(vm);

    lt_Value key = lt_make_string(vm, "loadLibrary");
    lt_Value value = lt_table_get(vm, vm->global, key);
    if (!LT_IS_NULL(value))
    {
        lt_destroy(vm);
        return 2;
    }

    ltstd_open_loadlib(vm);
    value = lt_table_get(vm, vm->global, key);
    if (!LT_IS_NATIVE(value))
    {
        lt_destroy(vm);
        return 3;
    }

    lt_destroy(vm);
    return 0;
}
