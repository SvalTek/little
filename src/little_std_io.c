#include "little_std.h"

#include <stdio.h>
#include <time.h>

static uint8_t _lt_print(lt_VM* vm, uint8_t argc)
{
    for (int16_t i = argc - 1; i >= 0; --i)
    {
        char* str = ltstd_tostring(vm, vm->stack[vm->top - 1 - i]);
        printf("%s", str);
        vm->free(str);

        if (i > 0) printf(" ");
    }

    for (int16_t i = argc - 1; i >= 0; --i) lt_pop(vm);

    printf("\n");
    return 0;
}

static uint8_t _lt_clock(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to io.clock!");

    clock_t time = clock();
    double in_seconds = (double)time / (double)CLOCKS_PER_SEC;
    lt_push(vm, lt_make_number(in_seconds));

    return 1;
}

void ltstd_open_io(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    lt_table_set(vm, t, lt_make_string(vm, "print"), lt_make_native(vm, _lt_print));
    lt_table_set(vm, t, lt_make_string(vm, "clock"), lt_make_native(vm, _lt_clock));

    lt_table_set(vm, vm->global, lt_make_string(vm, "io"), t);
}
