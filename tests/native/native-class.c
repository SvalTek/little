#include "little.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    double value;
} NativeBox;

static const lt_Api* api;
static lt_Value native_box_class;
static lt_Value broken_box_class;
static lt_Value native_module;
static uint32_t destroy_count;
static uint32_t external_destroy_count;

static void destroy_box(void* data)
{
    if (((NativeBox*)data)->value == -2) puts("shutdown-finalizer");
    destroy_count++;
    free(data);
}

static NativeBox* box_data(lt_VM* vm, lt_Value instance)
{
    return api->instance_get_native_data(vm, instance, native_box_class);
}

static uint8_t box_constructor(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) api->runtime_error(vm, "NativeBox expects one value!");
    double value = api->get_number(api->pop(vm));
    lt_Value instance = api->pop(vm);
    NativeBox* box = malloc(sizeof(NativeBox));
    if (!box) api->runtime_error(vm, "NativeBox allocation failed!");
    box->value = value;
    api->instance_set_native_data(vm, instance, native_box_class, box);
    return 0;
}

static uint8_t broken_constructor(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "BrokenBox takes no values!");
    lt_Value instance = api->pop(vm);
    NativeBox* box = malloc(sizeof(NativeBox));
    if (!box) api->runtime_error(vm, "BrokenBox allocation failed!");
    box->value = -1;
    api->instance_set_native_data(vm, instance, broken_box_class, box);
    api->runtime_error(vm, "BrokenBox constructor failed!");
    return 0;
}

static void destroy_broken_box(void* data)
{
    destroy_count++;
    free(data);
}

static uint8_t box_add(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) api->runtime_error(vm, "add expects one value!");
    double amount = api->get_number(api->pop(vm));
    lt_Value instance = api->pop(vm);
    NativeBox* box = box_data(vm, instance);
    if (!box) api->runtime_error(vm, "NativeBox data was cleared!");
    box->value += amount;
    api->push(vm, api->make_number(box->value));
    return 1;
}

static uint8_t box_get_value(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "value getter expects no values!");
    NativeBox* box = box_data(vm, api->pop(vm));
    if (!box) api->runtime_error(vm, "NativeBox data was cleared!");
    api->push(vm, api->make_number(box->value));
    return 1;
}

static uint8_t box_set_value(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) api->runtime_error(vm, "value setter expects one value!");
    double value = api->get_number(api->pop(vm));
    NativeBox* box = box_data(vm, api->pop(vm));
    if (!box) api->runtime_error(vm, "NativeBox data was cleared!");
    box->value = value;
    return 0;
}

static uint8_t box_native_value(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "nativeValue expects no values!");
    NativeBox* box = box_data(vm, api->pop(vm));
    if (!box) api->runtime_error(vm, "NativeBox data was cleared!");
    api->push(vm, api->make_number(box->value));
    return 1;
}

static uint8_t box_class_context(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "classContext expects no values!");
    api->pop(vm);
    api->push(vm, vm->current && vm->current->class_context == LT_GET_OBJECT(native_box_class)
        ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t box_hidden(lt_VM* vm, uint8_t argc)
{
    while (argc--) api->pop(vm);
    api->push(vm, api->make_string(vm, "private"));
    return 1;
}

static uint8_t box_dispose(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "dispose expects no values!");
    api->instance_dispose_native_data(vm, api->pop(vm));
    return 0;
}

static uint8_t box_external_release(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "externalRelease expects no values!");
    lt_Value instance = api->pop(vm);
    NativeBox* box = box_data(vm, instance);
    if (box) {
        free(box);
        external_destroy_count++;
        api->instance_clear_native_data(vm, instance);
    }
    api->push(vm, LT_VALUE_TRUE);
    return 1;
}

static uint8_t drop_native_class(lt_VM* vm, uint8_t argc)
{
    while (argc--) api->pop(vm);
    api->table_set(vm, native_module, api->make_string(vm, "NativeBox"), LT_VALUE_NULL);
    return 0;
}

static uint8_t destroyed_count(lt_VM* vm, uint8_t argc)
{
    while (argc--) api->pop(vm);
    api->push(vm, api->make_number((double)destroy_count));
    return 1;
}

static uint8_t external_count(lt_VM* vm, uint8_t argc)
{
    while (argc--) api->pop(vm);
    api->push(vm, api->make_number((double)external_destroy_count));
    return 1;
}

static uint8_t raw_native_value(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) api->runtime_error(vm, "rawNativeValue expects one instance!");
    NativeBox* box = box_data(vm, api->pop(vm));
    if (!box) api->runtime_error(vm, "NativeBox data was cleared!");
    api->push(vm, api->make_number(box->value));
    return 1;
}

static uint8_t try_duplicate_registration(lt_VM* vm, uint8_t argc)
{
    while (argc--) api->pop(vm);
    api->class_add_method(vm, native_box_class, "value", box_native_value, LT_VIS_PUBLIC, 0);
    return 0;
}

static void set_native(lt_VM* vm, lt_Value module, const char* name, lt_NativeFn fn)
{
    api->table_set(vm, module, api->make_string(vm, name), api->make_native(vm, fn));
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* native_api)
{
    if (!native_api || native_api->version != LT_API_VERSION || native_api->size < sizeof(lt_Api))
        return LT_VALUE_NULL;
    api = native_api;
    native_module = api->make_table(vm);

    native_box_class = api->class_create(vm, "NativeBox");
    api->class_set_native_data_destroy(vm, native_box_class, destroy_box);
    api->class_set_constructor(vm, native_box_class, box_constructor);
    api->class_add_method(vm, native_box_class, "add", box_add, LT_VIS_PUBLIC, 0);
    api->class_add_method(vm, native_box_class, "nativeValue", box_native_value, LT_VIS_PUBLIC, 0);
    api->class_add_method(vm, native_box_class, "classContext", box_class_context, LT_VIS_PUBLIC, 0);
    api->class_add_method(vm, native_box_class, "hidden", box_hidden, LT_VIS_PRIVATE, 0);
    api->class_add_method(vm, native_box_class, "dispose", box_dispose, LT_VIS_PUBLIC, 0);
    api->class_add_method(vm, native_box_class, "externalRelease", box_external_release, LT_VIS_PUBLIC, 0);
    api->class_add_getter(vm, native_box_class, "value", box_get_value, LT_VIS_PUBLIC, 0);
    api->class_add_setter(vm, native_box_class, "value", box_set_value, LT_VIS_PUBLIC, 0);
    api->table_set(vm, native_module, api->make_string(vm, "NativeBox"), native_box_class);

    broken_box_class = api->class_create(vm, "BrokenBox");
    api->class_set_native_data_destroy(vm, broken_box_class, destroy_broken_box);
    api->class_set_constructor(vm, broken_box_class, broken_constructor);
    api->table_set(vm, native_module, api->make_string(vm, "BrokenBox"), broken_box_class);

    set_native(vm, native_module, "dropNativeClass", drop_native_class);
    set_native(vm, native_module, "destroyedCount", destroyed_count);
    set_native(vm, native_module, "externalCount", external_count);
    set_native(vm, native_module, "tryDuplicateRegistration", try_duplicate_registration);
    set_native(vm, native_module, "rawNativeValue", raw_native_value);
    return native_module;
}
