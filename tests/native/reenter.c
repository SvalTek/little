#include "little.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

static const lt_Api* lt = 0;

/* Reproduces the webui dispatch_events pattern: a native function that
   re-enters the VM through lt->exec with a callable that raises a runtime
   error. The outer native frame must survive the unwind so the interpreter
   loop can pop it after the native returns. Previously lt_exec zeroed
   vm->depth/vm->top/vm->current on error, which made the interpreter's
   --vm->depth wrap to 65535 and read callstack[65534] -> crash. */
static uint8_t reenter_run(lt_VM* vm, uint8_t argc)
{
    if (argc != 1)
    {
        while (argc--) lt->pop(vm);
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value callable = lt->pop(vm);
    lt->push(vm, callable); /* argument for the reentered call */
    uint16_t returns = lt->exec(vm, callable, 1);
    if (returns > 0) lt->pop(vm);

    lt->push(vm, lt->make_number((double)returns));
    return 1;
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api)) return LT_VALUE_NULL;
    lt = api;
    lt_Value module = lt->make_table(vm);
    lt->table_set(vm, module, lt->make_string(vm, "run"), lt->make_native(vm, reenter_run));
    return module;
}
