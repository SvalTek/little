#include "../../src/little.h"
#include "../../src/little_std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_error(lt_VM* vm, const char* message)
{
    (void)vm;
    fprintf(stderr, "%s\n", message);
}

static lt_ModuleLoaderResult load_virtual_module(lt_VM* vm, const char* requested, char** source, char** module_name, void* userdata)
{
    (void)userdata;
    if (strcmp(requested, "virtual/greeting") != 0)
        return LT_MODULE_LOADER_NOT_FOUND;

    *source = vm->alloc(strlen("return { message: \"from loader\" }") + 1);
    strcpy(*source, "return { message: \"from loader\" }");
    *module_name = vm->alloc(strlen("virtual/greeting.little") + 1);
    strcpy(*module_name, "virtual/greeting.little");
    return LT_MODULE_LOADER_FOUND;
}

int main(void)
{
    lt_VM* vm = lt_open(malloc, free, test_error);
    if (!vm) return 1;

    lt_add_module_loader(vm, load_virtual_module, 0);
    ltstd_open_all(vm);

    if (lt_dostring(vm, "return import \"virtual/greeting\"", "<module-loader-test>") != 1)
    {
        lt_destroy(vm);
        return 2;
    }

    lt_Value module = lt_pop(vm);
    lt_Value message = lt_table_get(vm, module, lt_make_string(vm, "message"));
    if (!LT_IS_STRING(message) || strcmp(lt_get_string(vm, message), "from loader") != 0)
    {
        lt_destroy(vm);
        return 3;
    }

    lt_destroy(vm);
    return 0;
}
