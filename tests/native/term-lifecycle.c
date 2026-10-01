#include "../../src/little.h"
#include "../../vendor/pdcursesmod/curses.h"

#include <stdint.h>
#include <stdio.h>

lt_Value ltopen(lt_VM* vm, const lt_Api* api);
void lt_term_shutdown(void);
void lt_term_test_set_active_poll_hook(lt_VM* vm, uint32_t poll_hook);
void lt_term_test_set_callback(lt_VM* vm, lt_Value callback);

static lt_VM test_vms[24];
static lt_VM* removed_vm;
static uint32_t removed_hook;
static uint32_t persistent_roots;
static uint32_t persistent_unroots;

static lt_Value test_make_string(lt_VM* vm, const char* value)
{
    (void)vm;
    (void)value;
    return LT_VALUE_TRUE;
}

static lt_Value test_make_table(lt_VM* vm)
{
    (void)vm;
    return LT_VALUE_TRUE;
}

static lt_Value test_make_native(lt_VM* vm, lt_NativeFn fn)
{
    (void)vm;
    (void)fn;
    return LT_VALUE_TRUE;
}

static lt_Value test_table_set(lt_VM* vm, lt_Value table, lt_Value key, lt_Value value)
{
    (void)vm;
    (void)table;
    (void)key;
    (void)value;
    return LT_VALUE_TRUE;
}

static void test_remove_poll_hook(lt_VM* vm, uint32_t hook_id)
{
    removed_vm = vm;
    removed_hook = hook_id;
}

static void test_root_persistent(lt_VM* vm, lt_Value value)
{
    (void)vm;
    (void)value;
    persistent_roots++;
}

static void test_unroot_persistent(lt_VM* vm, lt_Value value)
{
    (void)vm;
    (void)value;
    persistent_unroots++;
}

static const lt_Api test_api = {
    .version = LT_API_VERSION,
    .size = sizeof(lt_Api),
    .make_string = test_make_string,
    .make_table = test_make_table,
    .make_native = test_make_native,
    .table_set = test_table_set,
    .remove_poll_hook = test_remove_poll_hook,
    .root_persistent = test_root_persistent,
    .unroot_persistent = test_unroot_persistent,
};

int endwin(void)
{
    return 0;
}

int main(void)
{
    for (size_t i = 0; i < sizeof(test_vms) / sizeof(test_vms[0]); ++i)
    {
        if (ltopen(&test_vms[i], &test_api) == LT_VALUE_NULL)
        {
            fprintf(stderr, "term module registration failed at VM %zu\n", i);
            return 1;
        }
    }

    lt_term_shutdown();

    for (size_t i = 0; i < sizeof(test_vms) / sizeof(test_vms[0]); ++i)
    {
        if (ltopen(&test_vms[i], &test_api) == LT_VALUE_NULL)
        {
            fprintf(stderr, "term module registration failed after shutdown at VM %zu\n", i);
            return 2;
        }
    }

    removed_vm = 0;
    removed_hook = 0;
    persistent_roots = 0;
    persistent_unroots = 0;
    lt_term_test_set_active_poll_hook(&test_vms[0], 73);
    lt_term_test_set_callback(&test_vms[0], LT_VALUE_TRUE);
    lt_term_shutdown();
    if (removed_vm != &test_vms[0] || removed_hook != 73)
    {
        fprintf(stderr, "terminal shutdown did not unregister its poll hook\n");
        return 3;
    }
    if (persistent_roots != 1 || persistent_unroots != 1)
    {
        fprintf(stderr, "terminal shutdown did not release its persistent callback root\n");
        return 4;
    }

    return 0;
}
