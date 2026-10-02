#include "little.h"

#include "raylib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#define RAY_MAX_COMMANDS 4096

typedef enum {
    RAY_CMD_RECT,
    RAY_CMD_TEXT
} RayCommandKind;

typedef struct {
    RayCommandKind kind;
    int x;
    int y;
    int w;
    int h;
    int size;
    Color color;
    char* text;
} RayCommand;

static const lt_Api* lt = 0;
static lt_VM* bound_vm = 0;
static lt_Value module_value = LT_VALUE_NULL;
static lt_Value callback_registry = LT_VALUE_NULL;
static lt_Value start_callback = LT_VALUE_NULL;
static lt_Value update_callback = LT_VALUE_NULL;
static uint8_t window_open = 0;
static uint8_t started = 0;
static uint8_t has_clear = 0;
static Color clear_color = { 0, 0, 0, 255 };
static RayCommand commands[RAY_MAX_COMMANDS];
static uint32_t command_count = 0;

/* raylib's value structs are exposed as native-backed Little classes. Little
   has a fixed type set, so each instance carries the struct as its native
   payload: `typeof v` is the class, fields dispatch through getters/setters,
   and the payload is freed by the class destroy callback on collection. */
typedef struct {
    double x;
    double y;
} LtVector2;

typedef struct {
    double x;
    double y;
    double z;
} LtVector3;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} LtColor;

typedef struct {
    double x;
    double y;
    double width;
    double height;
} LtRectangle;

static lt_Value vector2_class = LT_VALUE_NULL;
static lt_Value vector3_class = LT_VALUE_NULL;
static lt_Value color_class = LT_VALUE_NULL;
static lt_Value rectangle_class = LT_VALUE_NULL;

static void expect_number(lt_VM* vm, lt_Value value, const char* message)
{
    if (!LT_IS_NUMBER(value)) lt->runtime_error(vm, message);
}

static void expect_string(lt_VM* vm, lt_Value value, const char* message)
{
    if (!LT_IS_STRING(value)) lt->runtime_error(vm, message);
}

static void expect_callable(lt_VM* vm, lt_Value value, const char* message)
{
    if (LT_IS_OBJECT(value))
    {
        lt_ObjectType type = LT_GET_OBJECT(value)->type;
        if (type == LT_OBJECT_FN || type == LT_OBJECT_CLOSURE || type == LT_OBJECT_NATIVEFN ||
            type == LT_OBJECT_BOUND_NATIVE || type == LT_OBJECT_CLASS)
            return;
    }
    lt->runtime_error(vm, message);
}

static void destroy_native_data(void* data)
{
    free(data);
}

#define RAY_DATA_ACCESSOR(function_name, data_type, class_slot)                         \
    static data_type* function_name(lt_VM* vm, lt_Value value, const char* message)     \
    {                                                                                   \
        data_type* data = 0;                                                            \
        if (LT_IS_INSTANCE(value)) data = lt->instance_get_native_data(vm, value, class_slot); \
        if (!data) lt->runtime_error(vm, message);                                      \
        return data;                                                                    \
    }

RAY_DATA_ACCESSOR(vector2_data, LtVector2, vector2_class)
RAY_DATA_ACCESSOR(vector3_data, LtVector3, vector3_class)
RAY_DATA_ACCESSOR(color_data, LtColor, color_class)
RAY_DATA_ACCESSOR(rectangle_data, LtRectangle, rectangle_class)

static void* allocate_native_data(lt_VM* vm, size_t size)
{
    void* data = malloc(size);
    if (!data) lt->runtime_error(vm, "Out of memory!");
    return data;
}

/* Builds a native-class instance by re-entering the VM. Constructors push no
   return values, so a successful call yields exactly the instance. */
static lt_Value construct(lt_VM* vm, lt_Value klass, const double* args, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) lt->push(vm, lt->make_number(args[i]));
    uint16_t returns = lt->exec(vm, klass, count);
    if (returns == 0) return LT_VALUE_NULL;
    return lt->pop(vm);
}

static lt_Value make_vector2(lt_VM* vm, double x, double y)
{
    double args[2] = { x, y };
    return construct(vm, vector2_class, args, 2);
}

static lt_Value make_vector3(lt_VM* vm, double x, double y, double z)
{
    double args[3] = { x, y, z };
    return construct(vm, vector3_class, args, 3);
}

static lt_Value make_color(lt_VM* vm, double r, double g, double b, double a)
{
    double args[4] = { r, g, b, a };
    return construct(vm, color_class, args, 4);
}

static double table_field_number(lt_VM* vm, lt_Value table, const char* key, double fallback, const char* message)
{
    lt_Value value = lt->table_get(vm, table, lt->make_string(vm, key));
    if (LT_IS_NULL(value)) return fallback;
    if (!LT_IS_NUMBER(value)) lt->runtime_error(vm, message);
    return lt->get_number(value);
}

/* Fills `values`, seeded with defaults, from either one table argument or
   leading positional numbers. The instance is left for the constructor to pop. */
static void read_constructor_fields(lt_VM* vm, uint8_t argc, const char* type_name, const char* const* keys, uint8_t field_count, double* values)
{
    uint8_t count = (uint8_t)(argc - 1);
    char message[96];
    if (count == 1)
    {
        lt_Value source = lt->pop(vm);
        if (LT_IS_TABLE(source))
        {
            for (uint8_t i = 0; i < field_count; ++i)
            {
                snprintf(message, sizeof(message), "Expected %s %s to be number!", type_name, keys[i]);
                values[i] = table_field_number(vm, source, keys[i], values[i], message);
            }
            return;
        }
        snprintf(message, sizeof(message), "Expected %s %s to be number!", type_name, keys[0]);
        expect_number(vm, source, message);
        values[0] = lt->get_number(source);
        return;
    }
    if (count == 0) return;
    if (count > field_count)
    {
        snprintf(message, sizeof(message), "Expected at most %u values for %s!", field_count, type_name);
        lt->runtime_error(vm, message);
    }
    lt_Value raw[4];
    for (uint8_t i = count; i > 0; --i) raw[i - 1] = lt->pop(vm);
    for (uint8_t i = 0; i < count; ++i)
    {
        snprintf(message, sizeof(message), "Expected %s %s to be number!", type_name, keys[i]);
        expect_number(vm, raw[i], message);
        values[i] = lt->get_number(raw[i]);
    }
}

static uint8_t vector2_constructor(lt_VM* vm, uint8_t argc)
{
    static const char* const keys[] = { "x", "y" };
    if (argc > 3) lt->runtime_error(vm, "Expected a table or x, y numbers for Vector2!");
    double values[2] = { 0, 0 };
    read_constructor_fields(vm, argc, "Vector2", keys, 2, values);
    lt_Value instance = lt->pop(vm);
    LtVector2* data = allocate_native_data(vm, sizeof(LtVector2));
    data->x = values[0];
    data->y = values[1];
    lt->instance_set_native_data(vm, instance, vector2_class, data);
    return 0;
}

static uint8_t vector3_constructor(lt_VM* vm, uint8_t argc)
{
    static const char* const keys[] = { "x", "y", "z" };
    if (argc > 4) lt->runtime_error(vm, "Expected a table or x, y, z numbers for Vector3!");
    double values[3] = { 0, 0, 0 };
    read_constructor_fields(vm, argc, "Vector3", keys, 3, values);
    lt_Value instance = lt->pop(vm);
    LtVector3* data = allocate_native_data(vm, sizeof(LtVector3));
    data->x = values[0];
    data->y = values[1];
    data->z = values[2];
    lt->instance_set_native_data(vm, instance, vector3_class, data);
    return 0;
}

static uint8_t color_constructor(lt_VM* vm, uint8_t argc)
{
    static const char* const keys[] = { "r", "g", "b", "a" };
    if (argc > 5) lt->runtime_error(vm, "Expected a table or r, g, b, a numbers for Color!");
    double values[4] = { 0, 0, 0, 255 };
    read_constructor_fields(vm, argc, "Color", keys, 4, values);
    lt_Value instance = lt->pop(vm);
    LtColor* data = allocate_native_data(vm, sizeof(LtColor));
    data->r = (uint8_t)(values[0] < 0 ? 0 : values[0] > 255 ? 255 : values[0]);
    data->g = (uint8_t)(values[1] < 0 ? 0 : values[1] > 255 ? 255 : values[1]);
    data->b = (uint8_t)(values[2] < 0 ? 0 : values[2] > 255 ? 255 : values[2]);
    data->a = (uint8_t)(values[3] < 0 ? 0 : values[3] > 255 ? 255 : values[3]);
    lt->instance_set_native_data(vm, instance, color_class, data);
    return 0;
}

static uint8_t rectangle_constructor(lt_VM* vm, uint8_t argc)
{
    static const char* const keys[] = { "x", "y", "width", "height" };
    if (argc > 5) lt->runtime_error(vm, "Expected a table or x, y, width, height numbers for Rectangle!");
    double values[4] = { 0, 0, 0, 0 };
    read_constructor_fields(vm, argc, "Rectangle", keys, 4, values);
    lt_Value instance = lt->pop(vm);
    LtRectangle* data = allocate_native_data(vm, sizeof(LtRectangle));
    data->x = values[0];
    data->y = values[1];
    data->width = values[2];
    data->height = values[3];
    lt->instance_set_native_data(vm, instance, rectangle_class, data);
    return 0;
}

#define RAY_NUMBER_GETTER(function_name, data_type, accessor, field, type_name)         \
    static uint8_t function_name(lt_VM* vm, uint8_t argc)                               \
    {                                                                                   \
        if (argc != 1) lt->runtime_error(vm, #field " getter expects no arguments!");    \
        data_type* data = accessor(vm, lt->pop(vm), "Expected a " type_name "!");        \
        lt->push(vm, lt->make_number(data->field));                                      \
        return 1;                                                                        \
    }

#define RAY_NUMBER_SETTER(function_name, data_type, accessor, field, type_name)         \
    static uint8_t function_name(lt_VM* vm, uint8_t argc)                               \
    {                                                                                   \
        if (argc != 2) lt->runtime_error(vm, #field " setter expects one value!");        \
        lt_Value raw = lt->pop(vm);                                                       \
        expect_number(vm, raw, "Expected a number for " #field "!");                      \
        double value = lt->get_number(raw);                                               \
        data_type* data = accessor(vm, lt->pop(vm), "Expected a " type_name "!");         \
        data->field = value;                                                              \
        return 0;                                                                          \
    }

#define RAY_CHANNEL_SETTER(function_name, field)                                        \
    static uint8_t function_name(lt_VM* vm, uint8_t argc)                               \
    {                                                                                   \
        if (argc != 2) lt->runtime_error(vm, #field " setter expects one value!");        \
        lt_Value raw = lt->pop(vm);                                                       \
        expect_number(vm, raw, "Expected a number for " #field "!");                      \
        double value = lt->get_number(raw);                                               \
        if (value < 0) value = 0;                                                         \
        if (value > 255) value = 255;                                                     \
        LtColor* data = color_data(vm, lt->pop(vm), "Expected a Color!");                 \
        data->field = (uint8_t)value;                                                     \
        return 0;                                                                          \
    }

RAY_NUMBER_GETTER(vector2_get_x, LtVector2, vector2_data, x, "Vector2")
RAY_NUMBER_SETTER(vector2_set_x, LtVector2, vector2_data, x, "Vector2")
RAY_NUMBER_GETTER(vector2_get_y, LtVector2, vector2_data, y, "Vector2")
RAY_NUMBER_SETTER(vector2_set_y, LtVector2, vector2_data, y, "Vector2")

RAY_NUMBER_GETTER(vector3_get_x, LtVector3, vector3_data, x, "Vector3")
RAY_NUMBER_SETTER(vector3_set_x, LtVector3, vector3_data, x, "Vector3")
RAY_NUMBER_GETTER(vector3_get_y, LtVector3, vector3_data, y, "Vector3")
RAY_NUMBER_SETTER(vector3_set_y, LtVector3, vector3_data, y, "Vector3")
RAY_NUMBER_GETTER(vector3_get_z, LtVector3, vector3_data, z, "Vector3")
RAY_NUMBER_SETTER(vector3_set_z, LtVector3, vector3_data, z, "Vector3")

RAY_NUMBER_GETTER(color_get_r, LtColor, color_data, r, "Color")
RAY_CHANNEL_SETTER(color_set_r, r)
RAY_NUMBER_GETTER(color_get_g, LtColor, color_data, g, "Color")
RAY_CHANNEL_SETTER(color_set_g, g)
RAY_NUMBER_GETTER(color_get_b, LtColor, color_data, b, "Color")
RAY_CHANNEL_SETTER(color_set_b, b)
RAY_NUMBER_GETTER(color_get_a, LtColor, color_data, a, "Color")
RAY_CHANNEL_SETTER(color_set_a, a)

RAY_NUMBER_GETTER(rectangle_get_x, LtRectangle, rectangle_data, x, "Rectangle")
RAY_NUMBER_SETTER(rectangle_set_x, LtRectangle, rectangle_data, x, "Rectangle")
RAY_NUMBER_GETTER(rectangle_get_y, LtRectangle, rectangle_data, y, "Rectangle")
RAY_NUMBER_SETTER(rectangle_set_y, LtRectangle, rectangle_data, y, "Rectangle")
RAY_NUMBER_GETTER(rectangle_get_width, LtRectangle, rectangle_data, width, "Rectangle")
RAY_NUMBER_SETTER(rectangle_set_width, LtRectangle, rectangle_data, width, "Rectangle")
RAY_NUMBER_GETTER(rectangle_get_height, LtRectangle, rectangle_data, height, "Rectangle")
RAY_NUMBER_SETTER(rectangle_set_height, LtRectangle, rectangle_data, height, "Rectangle")

static lt_Value make_boolean(uint8_t value)
{
    return value ? LT_VALUE_TRUE : LT_VALUE_FALSE;
}

static double vector2_length_value(LtVector2* data)
{
    return sqrt(data->x * data->x + data->y * data->y);
}

static uint8_t vector2_add(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "add expects one Vector2!");
    LtVector2* other = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, make_vector2(vm, self->x + other->x, self->y + other->y));
    return 1;
}

static uint8_t vector2_sub(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "sub expects one Vector2!");
    LtVector2* other = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, make_vector2(vm, self->x - other->x, self->y - other->y));
    return 1;
}

static uint8_t vector2_mul(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "mul expects one Vector2!");
    LtVector2* other = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, make_vector2(vm, self->x * other->x, self->y * other->y));
    return 1;
}

static uint8_t vector2_scale(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "scale expects one number!");
    lt_Value raw = lt->pop(vm);
    expect_number(vm, raw, "Expected a number to scale by!");
    double amount = lt->get_number(raw);
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, make_vector2(vm, self->x * amount, self->y * amount));
    return 1;
}

static uint8_t vector2_dot(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "dot expects one Vector2!");
    LtVector2* other = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, lt->make_number(self->x * other->x + self->y * other->y));
    return 1;
}

static uint8_t vector2_length(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "length expects no arguments!");
    lt->push(vm, lt->make_number(vector2_length_value(vector2_data(vm, lt->pop(vm), "Expected a Vector2!"))));
    return 1;
}

static uint8_t vector2_length_sq(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "lengthSq expects no arguments!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, lt->make_number(self->x * self->x + self->y * self->y));
    return 1;
}

static uint8_t vector2_normalize(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "normalize expects no arguments!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    double length = vector2_length_value(self);
    if (length == 0) lt->push(vm, make_vector2(vm, 0, 0));
    else lt->push(vm, make_vector2(vm, self->x / length, self->y / length));
    return 1;
}

static uint8_t vector2_distance(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "distance expects one Vector2!");
    LtVector2* other = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    double dx = self->x - other->x;
    double dy = self->y - other->y;
    lt->push(vm, lt->make_number(sqrt(dx * dx + dy * dy)));
    return 1;
}

static uint8_t vector2_clone(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "clone expects no arguments!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, make_vector2(vm, self->x, self->y));
    return 1;
}

static uint8_t vector2_equals(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "equals expects one Vector2!");
    LtVector2* other = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    lt->push(vm, make_boolean(self->x == other->x && self->y == other->y));
    return 1;
}

static uint8_t vector2_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    char text[64];
    snprintf(text, sizeof(text), "(%g, %g)", self->x, self->y);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static double vector3_length_value(LtVector3* data)
{
    return sqrt(data->x * data->x + data->y * data->y + data->z * data->z);
}

static uint8_t vector3_add(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "add expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, make_vector3(vm, self->x + other->x, self->y + other->y, self->z + other->z));
    return 1;
}

static uint8_t vector3_sub(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "sub expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, make_vector3(vm, self->x - other->x, self->y - other->y, self->z - other->z));
    return 1;
}

static uint8_t vector3_mul(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "mul expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, make_vector3(vm, self->x * other->x, self->y * other->y, self->z * other->z));
    return 1;
}

static uint8_t vector3_scale(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "scale expects one number!");
    lt_Value raw = lt->pop(vm);
    expect_number(vm, raw, "Expected a number to scale by!");
    double amount = lt->get_number(raw);
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, make_vector3(vm, self->x * amount, self->y * amount, self->z * amount));
    return 1;
}

static uint8_t vector3_dot(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "dot expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, lt->make_number(self->x * other->x + self->y * other->y + self->z * other->z));
    return 1;
}

static uint8_t vector3_cross(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "cross expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    double x = self->y * other->z - self->z * other->y;
    double y = self->z * other->x - self->x * other->z;
    double z = self->x * other->y - self->y * other->x;
    lt->push(vm, make_vector3(vm, x, y, z));
    return 1;
}

static uint8_t vector3_length(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "length expects no arguments!");
    lt->push(vm, lt->make_number(vector3_length_value(vector3_data(vm, lt->pop(vm), "Expected a Vector3!"))));
    return 1;
}

static uint8_t vector3_length_sq(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "lengthSq expects no arguments!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, lt->make_number(self->x * self->x + self->y * self->y + self->z * self->z));
    return 1;
}

static uint8_t vector3_normalize(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "normalize expects no arguments!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    double length = vector3_length_value(self);
    if (length == 0) lt->push(vm, make_vector3(vm, 0, 0, 0));
    else lt->push(vm, make_vector3(vm, self->x / length, self->y / length, self->z / length));
    return 1;
}

static uint8_t vector3_distance(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "distance expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    double dx = self->x - other->x;
    double dy = self->y - other->y;
    double dz = self->z - other->z;
    lt->push(vm, lt->make_number(sqrt(dx * dx + dy * dy + dz * dz)));
    return 1;
}

static uint8_t vector3_clone(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "clone expects no arguments!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, make_vector3(vm, self->x, self->y, self->z));
    return 1;
}

static uint8_t vector3_equals(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "equals expects one Vector3!");
    LtVector3* other = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    lt->push(vm, make_boolean(self->x == other->x && self->y == other->y && self->z == other->z));
    return 1;
}

static uint8_t vector3_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    char text[80];
    snprintf(text, sizeof(text), "(%g, %g, %g)", self->x, self->y, self->z);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t color_equals(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "equals expects one Color!");
    LtColor* other = color_data(vm, lt->pop(vm), "Expected a Color!");
    LtColor* self = color_data(vm, lt->pop(vm), "Expected a Color!");
    lt->push(vm, make_boolean(self->r == other->r && self->g == other->g && self->b == other->b && self->a == other->a));
    return 1;
}

static uint8_t color_with_alpha(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "withAlpha expects one number!");
    lt_Value raw = lt->pop(vm);
    expect_number(vm, raw, "Expected an alpha number!");
    double alpha = lt->get_number(raw);
    LtColor* self = color_data(vm, lt->pop(vm), "Expected a Color!");
    lt->push(vm, make_color(vm, self->r, self->g, self->b, alpha));
    return 1;
}

static uint8_t color_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtColor* self = color_data(vm, lt->pop(vm), "Expected a Color!");
    char text[64];
    snprintf(text, sizeof(text), "rgba(%u, %u, %u, %u)", (unsigned)self->r, (unsigned)self->g, (unsigned)self->b, (unsigned)self->a);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t rectangle_equals(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "equals expects one Rectangle!");
    LtRectangle* other = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle!");
    LtRectangle* self = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle!");
    lt->push(vm, make_boolean(self->x == other->x && self->y == other->y && self->width == other->width && self->height == other->height));
    return 1;
}

static uint8_t rectangle_contains(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "contains expects one Vector2!");
    LtVector2* point = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    LtRectangle* self = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle!");
    lt->push(vm, make_boolean(point->x >= self->x && point->x <= self->x + self->width
        && point->y >= self->y && point->y <= self->y + self->height));
    return 1;
}

static uint8_t rectangle_center(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "center expects no arguments!");
    LtRectangle* self = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle!");
    lt->push(vm, make_vector2(vm, self->x + self->width / 2.0, self->y + self->height / 2.0));
    return 1;
}

static uint8_t rectangle_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtRectangle* self = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle!");
    char text[96];
    snprintf(text, sizeof(text), "(%g, %g, %g, %g)", self->x, self->y, self->width, self->height);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static Color expect_color(lt_VM* vm, lt_Value value, const char* message)
{
    LtColor* source = color_data(vm, value, message);
    Color color;
    color.r = source->r;
    color.g = source->g;
    color.b = source->b;
    color.a = source->a;
    return color;
}

static void clear_commands(void)
{
    for (uint32_t i = 0; i < command_count; ++i)
    {
        if (commands[i].kind == RAY_CMD_TEXT && commands[i].text)
        {
            free(commands[i].text);
            commands[i].text = 0;
        }
    }
    command_count = 0;
}

static void require_draw_target(lt_VM* vm)
{
    if (!window_open) lt->runtime_error(vm, "Expected ray.open before drawing!");
    if (command_count >= RAY_MAX_COMMANDS) lt->runtime_error(vm, "Too many ray draw commands in one frame!");
}

static void push_command(RayCommand command)
{
    commands[command_count++] = command;
}

static uint8_t native_open(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected width, height, and title for ray.open!");
    lt_Value title = lt->pop(vm);
    lt_Value height = lt->pop(vm);
    lt_Value width = lt->pop(vm);
    expect_number(vm, width, "Expected ray window width to be number!");
    expect_number(vm, height, "Expected ray window height to be number!");
    expect_string(vm, title, "Expected ray window title to be string!");
    if (window_open) lt->runtime_error(vm, "Ray window is already open!");
    InitWindow((int)lt->get_number(width), (int)lt->get_number(height), lt->get_string(vm, title));
    if (!IsWindowReady()) lt->runtime_error(vm, "Failed to initialize the ray window!");
    window_open = 1;
    started = 0;
    clear_commands();
    has_clear = 0;
    return 0;
}

static uint8_t native_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.close!");
    if (window_open)
    {
        CloseWindow();
        window_open = 0;
        clear_commands();
        has_clear = 0;
    }
    return 0;
}

static void run_callback(lt_VM* vm, lt_Value callback, uint8_t argc)
{
    uint16_t returns = lt->exec(vm, callback, argc);
    while (returns--) lt->pop(vm);
}

static uint8_t native_on(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected name and callback for ray.on!");
    lt_Value callback = lt->pop(vm);
    lt_Value name = lt->pop(vm);
    expect_string(vm, name, "Expected ray callback name to be string!");
    expect_callable(vm, callback, "Expected ray callback to be callable!");
    const char* event = lt->get_string(vm, name);
    if (strcmp(event, "start") == 0) start_callback = callback;
    else if (strcmp(event, "update") == 0) update_callback = callback;
    else lt->runtime_error(vm, "Unknown ray callback! Expected start or update.");
    lt->table_set(vm, callback_registry, name, callback);
    return 0;
}

static uint8_t native_update(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.update!");
    if (!window_open) lt->runtime_error(vm, "Expected ray.open before ray.update!");
    if (WindowShouldClose())
    {
        clear_commands();
        has_clear = 0;
        lt->push(vm, LT_VALUE_FALSE);
        return 1;
    }
    if (!started)
    {
        started = 1;
        if (!LT_IS_NULL(start_callback)) run_callback(vm, start_callback, 0);
    }
    /* A callback may close the window; never touch the GL context after that. */
    if (window_open)
    {
        float dt = GetFrameTime();
        if (!LT_IS_NULL(update_callback))
        {
            lt->push(vm, lt->make_number((double)dt));
            run_callback(vm, update_callback, 1);
        }
    }
    if (!window_open)
    {
        clear_commands();
        has_clear = 0;
        lt->push(vm, LT_VALUE_FALSE);
        return 1;
    }
    BeginDrawing();
    if (has_clear) ClearBackground(clear_color);
    else ClearBackground(BLACK);
    has_clear = 0;
    for (uint32_t i = 0; i < command_count; ++i)
    {
        RayCommand* command = &commands[i];
        if (command->kind == RAY_CMD_RECT)
            DrawRectangle(command->x, command->y, command->w, command->h, command->color);
        else
        {
            DrawText(command->text, command->x, command->y, command->size, command->color);
            free(command->text);
            command->text = 0;
        }
    }
    EndDrawing();
    clear_commands();
    lt->poll(vm);
    lt->push(vm, LT_VALUE_TRUE);
    return 1;
}

static uint8_t native_clear(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a Color for ray.clear!");
    lt_Value color = lt->pop(vm);
    if (!window_open) lt->runtime_error(vm, "Expected ray.open before ray.clear!");
    clear_color = expect_color(vm, color, "Expected a Color for ray.clear!");
    has_clear = 1;
    return 0;
}

static uint8_t native_rect(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Rectangle and a Color for ray.rect!");
    lt_Value color_value = lt->pop(vm);
    lt_Value bounds_value = lt->pop(vm);
    Color color = expect_color(vm, color_value, "Expected a Color for ray.rect!");
    LtRectangle* bounds = rectangle_data(vm, bounds_value, "Expected a Rectangle for ray.rect!");
    RayCommand command;
    command.kind = RAY_CMD_RECT;
    command.x = (int)bounds->x;
    command.y = (int)bounds->y;
    command.w = (int)bounds->width;
    command.h = (int)bounds->height;
    command.size = 0;
    command.color = color;
    command.text = 0;
    require_draw_target(vm);
    push_command(command);
    return 0;
}

static uint8_t native_text(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected text, a Vector2, a size, and a Color for ray.text!");
    lt_Value color_value = lt->pop(vm);
    lt_Value size_value = lt->pop(vm);
    lt_Value position_value = lt->pop(vm);
    lt_Value message = lt->pop(vm);
    expect_string(vm, message, "Expected ray text to be string!");
    expect_number(vm, size_value, "Expected ray text size to be number!");
    int size = (int)lt->get_number(size_value);
    if (size <= 0) lt->runtime_error(vm, "Expected ray text size to be positive!");
    LtVector2* position = vector2_data(vm, position_value, "Expected a Vector2 for ray.text!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.text!");
    require_draw_target(vm);
    RayCommand command;
    command.kind = RAY_CMD_TEXT;
    command.x = (int)position->x;
    command.y = (int)position->y;
    command.w = 0;
    command.h = 0;
    command.size = size;
    command.color = color;
    const char* source = lt->get_string(vm, message);
    command.text = malloc(strlen(source) + 1);
    if (!command.text) lt->runtime_error(vm, "Out of memory!");
    memcpy(command.text, source, strlen(source) + 1);
    push_command(command);
    return 0;
}

static uint8_t native_key_pressed(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected key code for ray.keyPressed!");
    lt_Value key = lt->pop(vm);
    expect_number(vm, key, "Expected ray key code to be number!");
    lt->push(vm, IsKeyPressed((int)lt->get_number(key)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_key_down(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected key code for ray.keyDown!");
    lt_Value key = lt->pop(vm);
    expect_number(vm, key, "Expected ray key code to be number!");
    lt->push(vm, IsKeyDown((int)lt->get_number(key)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_mouse_pressed(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected mouse button for ray.mousePressed!");
    lt_Value button = lt->pop(vm);
    expect_number(vm, button, "Expected ray mouse button to be number!");
    lt->push(vm, IsMouseButtonPressed((int)lt->get_number(button)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_mouse_position(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.mouse!");
    lt->push(vm, make_vector2(vm, (double)GetMouseX(), (double)GetMouseY()));
    return 1;
}

static uint8_t native_set_fps(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected fps for ray.setFPS!");
    lt_Value fps = lt->pop(vm);
    expect_number(vm, fps, "Expected ray fps to be number!");
    SetTargetFPS((int)lt->get_number(fps));
    return 0;
}

static void set_native(lt_VM* vm, lt_Value module, const char* name, lt_NativeFn fn)
{
    lt->table_set(vm, module, lt->make_string(vm, name), lt->make_native(vm, fn));
}

static void set_number(lt_VM* vm, lt_Value module, const char* name, double value)
{
    lt->table_set(vm, module, lt->make_string(vm, name), lt->make_number(value));
}

static void set_color(lt_VM* vm, lt_Value colors, const char* name, double r, double g, double b, double a)
{
    lt->table_set(vm, colors, lt->make_string(vm, name), make_color(vm, r, g, b, a));
}

#define RAY_CLASS_METHOD(klass, name, fn) lt->class_add_method(vm, klass, name, fn, LT_VIS_PUBLIC, 0)
#define RAY_CLASS_GETTER(klass, name, fn) lt->class_add_getter(vm, klass, name, fn, LT_VIS_PUBLIC, 0)
#define RAY_CLASS_SETTER(klass, name, fn) lt->class_add_setter(vm, klass, name, fn, LT_VIS_PUBLIC, 0)

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api))
        return LT_VALUE_NULL;

    /* Raylib owns a single process-global window, so it cannot serve two VMs. */
    if (bound_vm) return bound_vm == vm ? module_value : LT_VALUE_NULL;

    lt = api;
    bound_vm = vm;
    start_callback = LT_VALUE_NULL;
    update_callback = LT_VALUE_NULL;
    module_value = lt->make_table(vm);
    callback_registry = lt->make_table(vm);
    lt->table_set(vm, module_value, lt->make_string(vm, "__callbacks"), callback_registry);

    vector2_class = lt->class_create(vm, "Vector2");
    lt->class_set_native_data_destroy(vm, vector2_class, destroy_native_data);
    lt->class_set_constructor(vm, vector2_class, vector2_constructor);
    RAY_CLASS_GETTER(vector2_class, "x", vector2_get_x);
    RAY_CLASS_SETTER(vector2_class, "x", vector2_set_x);
    RAY_CLASS_GETTER(vector2_class, "y", vector2_get_y);
    RAY_CLASS_SETTER(vector2_class, "y", vector2_set_y);
    RAY_CLASS_METHOD(vector2_class, "add", vector2_add);
    RAY_CLASS_METHOD(vector2_class, "sub", vector2_sub);
    RAY_CLASS_METHOD(vector2_class, "mul", vector2_mul);
    RAY_CLASS_METHOD(vector2_class, "scale", vector2_scale);
    RAY_CLASS_METHOD(vector2_class, "dot", vector2_dot);
    RAY_CLASS_METHOD(vector2_class, "length", vector2_length);
    RAY_CLASS_METHOD(vector2_class, "lengthSq", vector2_length_sq);
    RAY_CLASS_METHOD(vector2_class, "normalize", vector2_normalize);
    RAY_CLASS_METHOD(vector2_class, "distance", vector2_distance);
    RAY_CLASS_METHOD(vector2_class, "clone", vector2_clone);
    RAY_CLASS_METHOD(vector2_class, "equals", vector2_equals);
    RAY_CLASS_METHOD(vector2_class, "toString", vector2_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Vector2"), vector2_class);

    vector3_class = lt->class_create(vm, "Vector3");
    lt->class_set_native_data_destroy(vm, vector3_class, destroy_native_data);
    lt->class_set_constructor(vm, vector3_class, vector3_constructor);
    RAY_CLASS_GETTER(vector3_class, "x", vector3_get_x);
    RAY_CLASS_SETTER(vector3_class, "x", vector3_set_x);
    RAY_CLASS_GETTER(vector3_class, "y", vector3_get_y);
    RAY_CLASS_SETTER(vector3_class, "y", vector3_set_y);
    RAY_CLASS_GETTER(vector3_class, "z", vector3_get_z);
    RAY_CLASS_SETTER(vector3_class, "z", vector3_set_z);
    RAY_CLASS_METHOD(vector3_class, "add", vector3_add);
    RAY_CLASS_METHOD(vector3_class, "sub", vector3_sub);
    RAY_CLASS_METHOD(vector3_class, "mul", vector3_mul);
    RAY_CLASS_METHOD(vector3_class, "scale", vector3_scale);
    RAY_CLASS_METHOD(vector3_class, "dot", vector3_dot);
    RAY_CLASS_METHOD(vector3_class, "cross", vector3_cross);
    RAY_CLASS_METHOD(vector3_class, "length", vector3_length);
    RAY_CLASS_METHOD(vector3_class, "lengthSq", vector3_length_sq);
    RAY_CLASS_METHOD(vector3_class, "normalize", vector3_normalize);
    RAY_CLASS_METHOD(vector3_class, "distance", vector3_distance);
    RAY_CLASS_METHOD(vector3_class, "clone", vector3_clone);
    RAY_CLASS_METHOD(vector3_class, "equals", vector3_equals);
    RAY_CLASS_METHOD(vector3_class, "toString", vector3_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Vector3"), vector3_class);

    color_class = lt->class_create(vm, "Color");
    lt->class_set_native_data_destroy(vm, color_class, destroy_native_data);
    lt->class_set_constructor(vm, color_class, color_constructor);
    RAY_CLASS_GETTER(color_class, "r", color_get_r);
    RAY_CLASS_SETTER(color_class, "r", color_set_r);
    RAY_CLASS_GETTER(color_class, "g", color_get_g);
    RAY_CLASS_SETTER(color_class, "g", color_set_g);
    RAY_CLASS_GETTER(color_class, "b", color_get_b);
    RAY_CLASS_SETTER(color_class, "b", color_set_b);
    RAY_CLASS_GETTER(color_class, "a", color_get_a);
    RAY_CLASS_SETTER(color_class, "a", color_set_a);
    RAY_CLASS_METHOD(color_class, "equals", color_equals);
    RAY_CLASS_METHOD(color_class, "withAlpha", color_with_alpha);
    RAY_CLASS_METHOD(color_class, "toString", color_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Color"), color_class);

    rectangle_class = lt->class_create(vm, "Rectangle");
    lt->class_set_native_data_destroy(vm, rectangle_class, destroy_native_data);
    lt->class_set_constructor(vm, rectangle_class, rectangle_constructor);
    RAY_CLASS_GETTER(rectangle_class, "x", rectangle_get_x);
    RAY_CLASS_SETTER(rectangle_class, "x", rectangle_set_x);
    RAY_CLASS_GETTER(rectangle_class, "y", rectangle_get_y);
    RAY_CLASS_SETTER(rectangle_class, "y", rectangle_set_y);
    RAY_CLASS_GETTER(rectangle_class, "width", rectangle_get_width);
    RAY_CLASS_SETTER(rectangle_class, "width", rectangle_set_width);
    RAY_CLASS_GETTER(rectangle_class, "height", rectangle_get_height);
    RAY_CLASS_SETTER(rectangle_class, "height", rectangle_set_height);
    RAY_CLASS_METHOD(rectangle_class, "equals", rectangle_equals);
    RAY_CLASS_METHOD(rectangle_class, "contains", rectangle_contains);
    RAY_CLASS_METHOD(rectangle_class, "center", rectangle_center);
    RAY_CLASS_METHOD(rectangle_class, "toString", rectangle_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Rectangle"), rectangle_class);

    set_native(vm, module_value, "open", native_open);
    set_native(vm, module_value, "close", native_close);
    set_native(vm, module_value, "on", native_on);
    set_native(vm, module_value, "update", native_update);
    set_native(vm, module_value, "clear", native_clear);
    set_native(vm, module_value, "rect", native_rect);
    set_native(vm, module_value, "text", native_text);
    set_native(vm, module_value, "keyPressed", native_key_pressed);
    set_native(vm, module_value, "keyDown", native_key_down);
    set_native(vm, module_value, "mousePressed", native_mouse_pressed);
    set_native(vm, module_value, "mouse", native_mouse_position);
    set_native(vm, module_value, "setFPS", native_set_fps);

    lt->table_set(vm, module_value, lt->make_string(vm, "version"), lt->make_string(vm, RAYLIB_VERSION));

    lt_Value keys = lt->make_table(vm);
    set_number(vm, keys, "space", 32);
    set_number(vm, keys, "enter", 257);
    set_number(vm, keys, "escape", 256);
    set_number(vm, keys, "left", 262);
    set_number(vm, keys, "right", 263);
    set_number(vm, keys, "up", 264);
    set_number(vm, keys, "down", 265);
    set_number(vm, keys, "w", 87);
    set_number(vm, keys, "a", 65);
    set_number(vm, keys, "s", 83);
    set_number(vm, keys, "d", 68);
    set_number(vm, keys, "mouseLeft", 0);
    set_number(vm, keys, "mouseRight", 1);
    set_number(vm, keys, "mouseMiddle", 2);
    lt->table_set(vm, module_value, lt->make_string(vm, "keys"), keys);

    lt_Value colors = lt->make_table(vm);
    set_color(vm, colors, "lightgray", 200, 200, 200, 255);
    set_color(vm, colors, "gray", 130, 130, 130, 255);
    set_color(vm, colors, "darkgray", 80, 80, 80, 255);
    set_color(vm, colors, "yellow", 253, 249, 0, 255);
    set_color(vm, colors, "gold", 255, 203, 0, 255);
    set_color(vm, colors, "orange", 255, 161, 0, 255);
    set_color(vm, colors, "pink", 255, 109, 194, 255);
    set_color(vm, colors, "red", 230, 41, 55, 255);
    set_color(vm, colors, "maroon", 190, 33, 55, 255);
    set_color(vm, colors, "green", 0, 228, 48, 255);
    set_color(vm, colors, "lime", 0, 158, 47, 255);
    set_color(vm, colors, "darkgreen", 0, 117, 44, 255);
    set_color(vm, colors, "skyblue", 102, 191, 255, 255);
    set_color(vm, colors, "blue", 0, 121, 241, 255);
    set_color(vm, colors, "darkblue", 0, 82, 172, 255);
    set_color(vm, colors, "purple", 200, 122, 255, 255);
    set_color(vm, colors, "violet", 135, 60, 190, 255);
    set_color(vm, colors, "darkpurple", 112, 31, 126, 255);
    set_color(vm, colors, "beige", 211, 176, 131, 255);
    set_color(vm, colors, "brown", 127, 106, 79, 255);
    set_color(vm, colors, "darkbrown", 76, 63, 47, 255);
    set_color(vm, colors, "white", 255, 255, 255, 255);
    set_color(vm, colors, "black", 0, 0, 0, 255);
    set_color(vm, colors, "blank", 0, 0, 0, 0);
    set_color(vm, colors, "magenta", 255, 0, 255, 255);
    set_color(vm, colors, "raywhite", 245, 245, 245, 255);
    lt->table_set(vm, module_value, lt->make_string(vm, "colors"), colors);

    return module_value;
}
