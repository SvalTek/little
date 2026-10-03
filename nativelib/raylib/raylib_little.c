#include "little.h"

#include "raylib.h"

/* raymath is header-only; raylib's own sources define RAYMATH_IMPLEMENTATION,
   so request file-local inline definitions for this translation unit only. */
#define RAYMATH_STATIC_INLINE
#include "raymath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#define RAY_MAX_COMMANDS 1024

typedef enum {
    RAY_CMD_RECT,
    RAY_CMD_TEXT,
    RAY_CMD_TEXTURE,
    RAY_CMD_TEXTURE_REC,
    RAY_CMD_TEXTURE_PRO,
    RAY_CMD_TEXT_EX,
    RAY_CMD_SHAPE,
    RAY_CMD_SHAPE3D,
    RAY_CMD_CLEAR,
    RAY_CMD_SCREENSHOT,
    RAY_CMD_BEGIN_2D,
    RAY_CMD_END_2D,
    RAY_CMD_BEGIN_3D,
    RAY_CMD_END_3D,
    RAY_CMD_BEGIN_TEXTURE,
    RAY_CMD_END_TEXTURE,
    RAY_CMD_MODEL,
    RAY_CMD_BILLBOARD
} RayCommandKind;

typedef enum {
    RAY_SHAPE_RECT_LINES,
    RAY_SHAPE_CIRCLE,
    RAY_SHAPE_CIRCLE_LINES,
    RAY_SHAPE_LINE,
    RAY_SHAPE_LINE_EX,
    RAY_SHAPE_TRIANGLE,
    RAY_SHAPE_TRIANGLE_LINES,
    RAY_SHAPE_POLYGON,
    RAY_SHAPE_POLYGON_LINES,
    RAY_SHAPE_RING
} RayShapeKind;

typedef struct {
    RayShapeKind shape;
    Rectangle bounds;
    Vector2 points[3];
    float radius;
    float radius2;
    float rotation;
    float thickness;
    int sides;
} RayShapeCommand;

typedef enum {
    RAY_SHAPE3D_CUBE,
    RAY_SHAPE3D_CUBE_WIRES,
    RAY_SHAPE3D_SPHERE,
    RAY_SHAPE3D_SPHERE_WIRES,
    RAY_SHAPE3D_CYLINDER,
    RAY_SHAPE3D_CYLINDER_WIRES,
    RAY_SHAPE3D_CAPSULE,
    RAY_SHAPE3D_GRID,
    RAY_SHAPE3D_LINE,
    RAY_SHAPE3D_POINT,
    RAY_SHAPE3D_TRIANGLE,
    RAY_SHAPE3D_PLANE,
    RAY_SHAPE3D_BOX
} RayShape3DKind;

/* 3D shapes carry three world points plus the handful of scalars the raylib
   cube/sphere/cylinder/capsule family takes, so one payload type covers them. */
typedef struct {
    RayShape3DKind shape;
    Vector3 points[3];
    Vector3 size;
    float radius;
    float radius2;
    float height;
    float thickness;
    int slices;
    int rings;
    int sides;
} RayShape3DCommand;

/* A model draw stores the model by value and the transform it was queued with;
   the instance stays alive through the queued-resource array. */
typedef struct {
    Model model;
    Vector3 position;
    Vector3 rotationAxis;
    Vector3 scale;
    float angle;
    uint8_t wires;
} RayModelCommand;

typedef struct {
    Camera3D camera;
    Texture2D texture;
    Vector3 position;
    float scale;
} RayBillboardCommand;

/* Texture draws carry their arguments in a heap payload so the fixed command
   buffer stays small. */
typedef struct {
    Texture2D texture;
    Rectangle source;
    Rectangle dest;
    Vector2 origin;
    Vector2 position;
    float rotation;
} RayTextureCommand;

typedef struct {
    Font font;
    Vector2 position;
    float size;
    float spacing;
} RayTextExCommand;

typedef struct {
    RayCommandKind kind;
    int x;
    int y;
    int w;
    int h;
    int size;
    Color color;
    char* text;
    void* payload;
    lt_Value resource;
} RayCommand;

static const lt_Api* lt = 0;
static lt_VM* bound_vm = 0;
static lt_Value module_value = LT_VALUE_NULL;
static lt_Value callback_registry = LT_VALUE_NULL;
static lt_Value start_callback = LT_VALUE_NULL;
static lt_Value update_callback = LT_VALUE_NULL;
static uint8_t window_open = 0;
static uint8_t started = 0;
static RayCommand commands[RAY_MAX_COMMANDS];
static uint32_t command_count = 0;

/* A deferred unload names its payload and kind directly: instance_get_native_data
   raises when asked for a class outside an instance's chain, so it cannot be
   used to probe which resource an instance holds. */
typedef struct {
    void* payload;
    uint8_t kind;
} RayDeferredUnload;

typedef enum {
    RAY_RESOURCE_TEXTURE,
    RAY_RESOURCE_FONT,
    RAY_RESOURCE_MESH,
    RAY_RESOURCE_MODEL,
    RAY_RESOURCE_RENDER_TEXTURE
} RayResourceKind;

static RayDeferredUnload deferred_unloads[RAY_MAX_COMMANDS];
static uint32_t deferred_unload_count = 0;

/* Resources referenced by queued draw commands are held in an array reachable
   from the module table, because a command's own reference lives in C memory
   the collector does not scan. */
static lt_Value queued_resources = LT_VALUE_NULL;

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

/* Fonts track ownership: raylib's default font must never be unloaded. */
typedef struct {
    Font font;
    uint8_t owned;
} LtFontData;

/* Meshes and models track ownership the same way: `modelFromMesh` hands the
   mesh to the model, and an unowned mesh is only good for reading its counts. */
typedef struct {
    Mesh mesh;
    uint8_t owned;
} LtMeshData;

typedef struct {
    Model model;
    uint8_t owned;
} LtModelData;

static lt_Value vector2_class = LT_VALUE_NULL;
static lt_Value vector3_class = LT_VALUE_NULL;
static lt_Value color_class = LT_VALUE_NULL;
static lt_Value rectangle_class = LT_VALUE_NULL;
static lt_Value image_class = LT_VALUE_NULL;
static lt_Value texture_class = LT_VALUE_NULL;
static lt_Value font_class = LT_VALUE_NULL;
static lt_Value camera2d_class = LT_VALUE_NULL;
static lt_Value camera3d_class = LT_VALUE_NULL;
static lt_Value ray_class = LT_VALUE_NULL;
static lt_Value ray_collision_class = LT_VALUE_NULL;
static lt_Value mesh_class = LT_VALUE_NULL;
static lt_Value model_class = LT_VALUE_NULL;
static lt_Value render_texture_class = LT_VALUE_NULL;
static uint32_t mode2d_depth = 0;
static uint32_t mode3d_depth = 0;
static uint32_t texture_depth = 0;

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
RAY_DATA_ACCESSOR(image_data, Image, image_class)
RAY_DATA_ACCESSOR(texture_data, Texture2D, texture_class)
RAY_DATA_ACCESSOR(font_data, LtFontData, font_class)
RAY_DATA_ACCESSOR(camera2d_data, Camera2D, camera2d_class)
RAY_DATA_ACCESSOR(camera3d_data, Camera3D, camera3d_class)
RAY_DATA_ACCESSOR(ray_data, Ray, ray_class)
RAY_DATA_ACCESSOR(ray_collision_data, RayCollision, ray_collision_class)
RAY_DATA_ACCESSOR(mesh_data, LtMeshData, mesh_class)
RAY_DATA_ACCESSOR(model_data, LtModelData, model_class)
RAY_DATA_ACCESSOR(render_texture_data, RenderTexture, render_texture_class)

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

static lt_Value make_rectangle(lt_VM* vm, double x, double y, double width, double height)
{
    double args[4] = { x, y, width, height };
    return construct(vm, rectangle_class, args, 4);
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
        data_type* data = accessor(vm, lt->pop(vm), "Expected " type_name "!");        \
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
        data_type* data = accessor(vm, lt->pop(vm), "Expected " type_name "!");         \
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

RAY_NUMBER_GETTER(vector2_get_x, LtVector2, vector2_data, x, "a Vector2")
RAY_NUMBER_SETTER(vector2_set_x, LtVector2, vector2_data, x, "a Vector2")
RAY_NUMBER_GETTER(vector2_get_y, LtVector2, vector2_data, y, "a Vector2")
RAY_NUMBER_SETTER(vector2_set_y, LtVector2, vector2_data, y, "a Vector2")

RAY_NUMBER_GETTER(vector3_get_x, LtVector3, vector3_data, x, "a Vector3")
RAY_NUMBER_SETTER(vector3_set_x, LtVector3, vector3_data, x, "a Vector3")
RAY_NUMBER_GETTER(vector3_get_y, LtVector3, vector3_data, y, "a Vector3")
RAY_NUMBER_SETTER(vector3_set_y, LtVector3, vector3_data, y, "a Vector3")
RAY_NUMBER_GETTER(vector3_get_z, LtVector3, vector3_data, z, "a Vector3")
RAY_NUMBER_SETTER(vector3_set_z, LtVector3, vector3_data, z, "a Vector3")

RAY_NUMBER_GETTER(color_get_r, LtColor, color_data, r, "Color")
RAY_CHANNEL_SETTER(color_set_r, r)
RAY_NUMBER_GETTER(color_get_g, LtColor, color_data, g, "Color")
RAY_CHANNEL_SETTER(color_set_g, g)
RAY_NUMBER_GETTER(color_get_b, LtColor, color_data, b, "Color")
RAY_CHANNEL_SETTER(color_set_b, b)
RAY_NUMBER_GETTER(color_get_a, LtColor, color_data, a, "Color")
RAY_CHANNEL_SETTER(color_set_a, a)

RAY_NUMBER_GETTER(rectangle_get_x, LtRectangle, rectangle_data, x, "a Rectangle")
RAY_NUMBER_SETTER(rectangle_set_x, LtRectangle, rectangle_data, x, "a Rectangle")
RAY_NUMBER_GETTER(rectangle_get_y, LtRectangle, rectangle_data, y, "a Rectangle")
RAY_NUMBER_SETTER(rectangle_set_y, LtRectangle, rectangle_data, y, "a Rectangle")
RAY_NUMBER_GETTER(rectangle_get_width, LtRectangle, rectangle_data, width, "a Rectangle")
RAY_NUMBER_SETTER(rectangle_set_width, LtRectangle, rectangle_data, width, "a Rectangle")
RAY_NUMBER_GETTER(rectangle_get_height, LtRectangle, rectangle_data, height, "a Rectangle")
RAY_NUMBER_SETTER(rectangle_set_height, LtRectangle, rectangle_data, height, "a Rectangle")

RAY_NUMBER_GETTER(image_get_width, Image, image_data, width, "an Image")
RAY_NUMBER_GETTER(image_get_height, Image, image_data, height, "an Image")

RAY_NUMBER_GETTER(texture_get_width, Texture2D, texture_data, width, "a Texture")
RAY_NUMBER_GETTER(texture_get_height, Texture2D, texture_data, height, "a Texture")

static uint8_t font_get_base_size(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "baseSize getter expects no arguments!");
    LtFontData* data = font_data(vm, lt->pop(vm), "Expected a Font!");
    lt->push(vm, lt->make_number((double)data->font.baseSize));
    return 1;
}

static uint8_t font_get_glyph_count(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "glyphCount getter expects no arguments!");
    LtFontData* data = font_data(vm, lt->pop(vm), "Expected a Font!");
    lt->push(vm, lt->make_number((double)data->font.glyphCount));
    return 1;
}

RAY_NUMBER_GETTER(camera2d_get_rotation, Camera2D, camera2d_data, rotation, "a Camera2D")
RAY_NUMBER_SETTER(camera2d_set_rotation, Camera2D, camera2d_data, rotation, "a Camera2D")
RAY_NUMBER_GETTER(camera2d_get_zoom, Camera2D, camera2d_data, zoom, "a Camera2D")
RAY_NUMBER_SETTER(camera2d_set_zoom, Camera2D, camera2d_data, zoom, "a Camera2D")

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

static uint8_t vector2_rotate(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "rotate expects one angle!");
    lt_Value angle_value = lt->pop(vm);
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    expect_number(vm, angle_value, "Expected a rotation angle number!");
    Vector2 source = { (float)self->x, (float)self->y };
    Vector2 result = Vector2Rotate(source, (float)lt->get_number(angle_value));
    lt->push(vm, make_vector2(vm, (double)result.x, (double)result.y));
    return 1;
}

static uint8_t vector2_lerp(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "lerp expects a Vector2 target and an amount!");
    lt_Value amount_value = lt->pop(vm);
    LtVector2* target = vector2_data(vm, lt->pop(vm), "Expected a Vector2 target!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    expect_number(vm, amount_value, "Expected a lerp amount number!");
    Vector2 source = { (float)self->x, (float)self->y };
    Vector2 end = { (float)target->x, (float)target->y };
    Vector2 result = Vector2Lerp(source, end, (float)lt->get_number(amount_value));
    lt->push(vm, make_vector2(vm, (double)result.x, (double)result.y));
    return 1;
}

static uint8_t vector2_reflect(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "reflect expects one normal Vector2!");
    LtVector2* normal = vector2_data(vm, lt->pop(vm), "Expected a Vector2 normal!");
    LtVector2* self = vector2_data(vm, lt->pop(vm), "Expected a Vector2!");
    Vector2 source = { (float)self->x, (float)self->y };
    Vector2 axis = { (float)normal->x, (float)normal->y };
    Vector2 result = Vector2Reflect(source, axis);
    lt->push(vm, make_vector2(vm, (double)result.x, (double)result.y));
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

static uint8_t vector3_lerp(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "lerp expects a Vector3 target and an amount!");
    lt_Value amount_value = lt->pop(vm);
    LtVector3* target = vector3_data(vm, lt->pop(vm), "Expected a Vector3 target!");
    LtVector3* self = vector3_data(vm, lt->pop(vm), "Expected a Vector3!");
    expect_number(vm, amount_value, "Expected a lerp amount number!");
    Vector3 source = { (float)self->x, (float)self->y, (float)self->z };
    Vector3 end = { (float)target->x, (float)target->y, (float)target->z };
    Vector3 result = Vector3Lerp(source, end, (float)lt->get_number(amount_value));
    lt->push(vm, make_vector3(vm, (double)result.x, (double)result.y, (double)result.z));
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

static Vector3 expect_vector3(lt_VM* vm, lt_Value value, const char* message)
{
    LtVector3* source = vector3_data(vm, value, message);
    Vector3 point;
    point.x = (float)source->x;
    point.y = (float)source->y;
    point.z = (float)source->z;
    return point;
}

static void clear_commands(lt_VM* vm)
{
    for (uint32_t i = 0; i < command_count; ++i)
    {
        free(commands[i].text);
        commands[i].text = 0;
        free(commands[i].payload);
        commands[i].payload = 0;
    }
    command_count = 0;
    mode2d_depth = 0;
    mode3d_depth = 0;
    texture_depth = 0;
    deferred_unload_count = 0;
    /* Release this frame's references to queued resources by swapping in a
       fresh array; the old one becomes collectable. */
    if (!LT_IS_NULL(queued_resources))
    {
        queued_resources = lt->make_array(vm);
        lt->table_set(vm, module_value, lt->make_string(vm, "__queued"), queued_resources);
    }
}

static void require_draw_target(lt_VM* vm)
{
    if (!window_open) lt->runtime_error(vm, "Expected ray.open before drawing!");
    if (command_count >= RAY_MAX_COMMANDS) lt->runtime_error(vm, "Too many ray draw commands in one frame!");
}

static RayCommand new_command(RayCommandKind kind)
{
    RayCommand command;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    /* Zeroed memory is not LT_VALUE_NULL, so seed the owning-resource slot. */
    command.resource = LT_VALUE_NULL;
    return command;
}

static void push_command(RayCommand command)
{
    commands[command_count++] = command;
}

static uint8_t table_field_bool(lt_VM* vm, lt_Value table, const char* key, uint8_t fallback)
{
    lt_Value value = lt->table_get(vm, table, lt->make_string(vm, key));
    if (LT_IS_NULL(value)) return fallback;
    if (!LT_IS_BOOL(value))
    {
        char message[96];
        snprintf(message, sizeof(message), "Expected ray window option %s to be boolean!", key);
        lt->runtime_error(vm, message);
    }
    return value == LT_VALUE_TRUE ? 1 : 0;
}

/* ---------------- resource types: Image, Texture, Font ---------------- */

static void unload_texture_payload(Texture2D* texture)
{
    if (texture->id > 0)
    {
        /* Unloading needs a live GL context; after ray.close the texture is
           already gone with the context. */
        if (IsWindowReady()) UnloadTexture(*texture);
        texture->id = 0;
        texture->width = 0;
        texture->height = 0;
        texture->mipmaps = 0;
        texture->format = 0;
    }
}

static void unload_font_payload(LtFontData* font)
{
    if (font->owned && font->font.texture.id > 0)
    {
        /* UnloadFont frees the glyph and rectangle arrays along with the atlas
           texture, so with the context gone only the id has to be dropped. */
        if (!IsWindowReady()) font->font.texture.id = 0;
        UnloadFont(font->font);
        memset(&font->font, 0, sizeof(Font));
    }
}

/* UnloadMesh frees the CPU arrays and the GPU buffers together, and raylib
   uploads a mesh as soon as it generates or loads it. After ray.close the GPU
   buffers are already gone with the context, so drop the GL handle first and
   let the call release the CPU side; skipping it would leak the vertex data. */
static void unload_mesh_payload(LtMeshData* mesh)
{
    if (mesh->owned && mesh->mesh.vertexCount > 0)
    {
        if (!IsWindowReady()) mesh->mesh.vaoId = 0;
        UnloadMesh(mesh->mesh);
        memset(&mesh->mesh, 0, sizeof(Mesh));
        mesh->owned = 0;
    }
}

/* UnloadModel walks its meshes, so clear their GL handles when the context is
   gone and let it release the CPU-side meshes, material maps, and arrays. */
static void unload_model_payload(LtModelData* model)
{
    if (model->owned && model->model.meshCount > 0)
    {
        if (!IsWindowReady())
        {
            for (int i = 0; i < model->model.meshCount; ++i) model->model.meshes[i].vaoId = 0;
        }
        UnloadModel(model->model);
        memset(&model->model, 0, sizeof(Model));
        model->owned = 0;
    }
}

static void unload_render_texture_payload(RenderTexture* target)
{
    if (target->id > 0)
    {
        if (IsWindowReady()) UnloadRenderTexture(*target);
        memset(target, 0, sizeof(RenderTexture));
    }
}

static void destroy_image(void* data)
{
    Image* image = data;
    if (image->data) UnloadImage(*image);
    free(image);
}

static void destroy_texture(void* data)
{
    unload_texture_payload(data);
    free(data);
}

static void destroy_font(void* data)
{
    unload_font_payload(data);
    free(data);
}

static void destroy_mesh(void* data)
{
    unload_mesh_payload(data);
    free(data);
}

static void destroy_model(void* data)
{
    unload_model_payload(data);
    free(data);
}

static void destroy_render_texture(void* data)
{
    unload_render_texture_payload(data);
    free(data);
}

static void require_window(lt_VM* vm, const char* message)
{
    if (!window_open) lt->runtime_error(vm, message);
}

/* Draw commands replay after the frame callback returns, so a resource a
   command references has to stay alive until then, and unloading it mid-frame
   is deferred until the command is finished with it. */
static uint8_t resource_is_queued(lt_VM* vm, lt_Value instance)
{
    for (uint32_t i = 0; i < command_count; ++i)
    {
        if (lt->equals(commands[i].resource, instance)) return 1;
    }
    return 0;
}

static void queue_resource(lt_VM* vm, lt_Value instance)
{
    /* The collector cannot see a command's C-side reference, so hold the
       instance where it can: rooted for this call, then in the module array. */
    lt->root(vm, instance);
    lt->array_push(vm, queued_resources, instance);
}

static void defer_unload(lt_VM* vm, void* payload, uint8_t kind)
{
    if (deferred_unload_count >= RAY_MAX_COMMANDS) lt->runtime_error(vm, "Too many deferred ray unloads in one frame!");
    deferred_unloads[deferred_unload_count].payload = payload;
    deferred_unloads[deferred_unload_count].kind = kind;
    deferred_unload_count++;
}

static void flush_deferred_unloads(void)
{
    for (uint32_t i = 0; i < deferred_unload_count; ++i)
    {
        switch (deferred_unloads[i].kind)
        {
        case RAY_RESOURCE_TEXTURE:
            unload_texture_payload(deferred_unloads[i].payload);
            break;
        case RAY_RESOURCE_FONT:
            unload_font_payload(deferred_unloads[i].payload);
            break;
        case RAY_RESOURCE_MESH:
            unload_mesh_payload(deferred_unloads[i].payload);
            break;
        case RAY_RESOURCE_MODEL:
            unload_model_payload(deferred_unloads[i].payload);
            break;
        case RAY_RESOURCE_RENDER_TEXTURE:
            unload_render_texture_payload(deferred_unloads[i].payload);
            break;
        }
    }
    deferred_unload_count = 0;
}

static void attach_image(lt_VM* vm, Image source)
{
    lt_Value instance = construct(vm, image_class, 0, 0);
    Image* data = allocate_native_data(vm, sizeof(Image));
    *data = source;
    lt->instance_set_native_data(vm, instance, image_class, data);
    lt->push(vm, instance);
}

static void attach_texture(lt_VM* vm, Texture2D source)
{
    lt_Value instance = construct(vm, texture_class, 0, 0);
    Texture2D* data = allocate_native_data(vm, sizeof(Texture2D));
    *data = source;
    lt->instance_set_native_data(vm, instance, texture_class, data);
    lt->push(vm, instance);
}

static void attach_font(lt_VM* vm, Font source, uint8_t owned)
{
    lt_Value instance = construct(vm, font_class, 0, 0);
    LtFontData* data = allocate_native_data(vm, sizeof(LtFontData));
    data->font = source;
    data->owned = owned;
    lt->instance_set_native_data(vm, instance, font_class, data);
    lt->push(vm, instance);
}

static uint8_t native_load_image(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a path for ray.loadImage!");
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected ray image path to be string!");
    Image source = LoadImage(lt->get_string(vm, path));
    if (!IsImageValid(source)) lt->runtime_error(vm, "Failed to load ray image!");
    attach_image(vm, source);
    return 1;
}

static uint8_t native_gen_image_color(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected width, height, and a Color for ray.genImageColor!");
    lt_Value color_value = lt->pop(vm);
    lt_Value height_value = lt->pop(vm);
    lt_Value width_value = lt->pop(vm);
    expect_number(vm, width_value, "Expected ray image width to be number!");
    expect_number(vm, height_value, "Expected ray image height to be number!");
    int width = (int)lt->get_number(width_value);
    int height = (int)lt->get_number(height_value);
    if (width <= 0 || height <= 0) lt->runtime_error(vm, "Expected positive ray image dimensions!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.genImageColor!");
    Image source = GenImageColor(width, height, color);
    if (!IsImageValid(source)) lt->runtime_error(vm, "Failed to generate ray image!");
    attach_image(vm, source);
    return 1;
}

static uint8_t native_load_texture(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a path for ray.loadTexture!");
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected ray texture path to be string!");
    require_window(vm, "Expected ray.open before ray.loadTexture!");
    Texture2D source = LoadTexture(lt->get_string(vm, path));
    if (!IsTextureValid(source)) lt->runtime_error(vm, "Failed to load ray texture!");
    attach_texture(vm, source);
    return 1;
}

static uint8_t native_load_texture_from_image(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected an Image for ray.loadTextureFromImage!");
    Image* image = image_data(vm, lt->pop(vm), "Expected an Image for ray.loadTextureFromImage!");
    require_window(vm, "Expected ray.open before ray.loadTextureFromImage!");
    if (!image->data) lt->runtime_error(vm, "Ray image has been unloaded!");
    Texture2D source = LoadTextureFromImage(*image);
    if (!IsTextureValid(source)) lt->runtime_error(vm, "Failed to create ray texture!");
    attach_texture(vm, source);
    return 1;
}

static uint8_t native_load_font(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a path and a size for ray.loadFont!");
    lt_Value size_value = lt->pop(vm);
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected ray font path to be string!");
    expect_number(vm, size_value, "Expected ray font size to be number!");
    int size = (int)lt->get_number(size_value);
    if (size <= 0) lt->runtime_error(vm, "Expected a positive ray font size!");
    require_window(vm, "Expected ray.open before ray.loadFont!");
    Font source = LoadFontEx(lt->get_string(vm, path), size, 0, 0);
    if (source.texture.id == 0) lt->runtime_error(vm, "Failed to load ray font!");
    attach_font(vm, source, 1);
    return 1;
}

static uint8_t native_default_font(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.defaultFont!");
    require_window(vm, "Expected ray.open before ray.defaultFont!");
    Font source = GetFontDefault();
    if (source.texture.id == 0) lt->runtime_error(vm, "Ray default font is unavailable!");
    attach_font(vm, source, 0);
    return 1;
}

static uint8_t native_window_size(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.windowSize!");
    require_window(vm, "Expected ray.open before ray.windowSize!");
    lt->push(vm, make_vector2(vm, (double)GetScreenWidth(), (double)GetScreenHeight()));
    return 1;
}

static uint8_t image_unload(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "unload expects no arguments!");
    Image* image = image_data(vm, lt->pop(vm), "Expected an Image!");
    if (image->data)
    {
        UnloadImage(*image);
        image->data = 0;
        image->width = 0;
        image->height = 0;
        image->mipmaps = 0;
        image->format = 0;
    }
    return 0;
}

static uint8_t image_export(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "export expects one path!");
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected an export path string!");
    Image* image = image_data(vm, lt->pop(vm), "Expected an Image!");
    if (!image->data) lt->runtime_error(vm, "Ray image has been unloaded!");
    lt->push(vm, ExportImage(*image, lt->get_string(vm, path)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t image_color_at(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "colorAt expects an x and a y!");
    lt_Value y_value = lt->pop(vm);
    lt_Value x_value = lt->pop(vm);
    Image* image = image_data(vm, lt->pop(vm), "Expected an Image!");
    expect_number(vm, x_value, "Expected a colorAt x number!");
    expect_number(vm, y_value, "Expected a colorAt y number!");
    if (!image->data) lt->runtime_error(vm, "Ray image has been unloaded!");
    int x = (int)lt->get_number(x_value);
    int y = (int)lt->get_number(y_value);
    if (x < 0 || y < 0 || x >= image->width || y >= image->height) lt->runtime_error(vm, "Expected colorAt coordinates inside the image!");
    Color color = GetImageColor(*image, x, y);
    lt->push(vm, make_color(vm, color.r, color.g, color.b, color.a));
    return 1;
}

static uint8_t image_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    Image* image = image_data(vm, lt->pop(vm), "Expected an Image!");
    char text[48];
    snprintf(text, sizeof(text), "image(%dx%d)", image->width, image->height);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t texture_unload(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "unload expects no arguments!");
    lt_Value instance = lt->pop(vm);
    Texture2D* texture = texture_data(vm, instance, "Expected a Texture!");
    if (resource_is_queued(vm, instance)) defer_unload(vm, texture, RAY_RESOURCE_TEXTURE);
    else unload_texture_payload(texture);
    return 0;
}

static RayTextureCommand* make_texture_command(lt_VM* vm, Texture2D* texture)
{
    if (texture->id == 0) lt->runtime_error(vm, "Ray texture has been unloaded!");
    require_draw_target(vm);
    RayTextureCommand* payload = allocate_native_data(vm, sizeof(RayTextureCommand));
    memset(payload, 0, sizeof(RayTextureCommand));
    payload->texture = *texture;
    return payload;
}

static void copy_rectangle(Rectangle* dest, LtRectangle* source)
{
    dest->x = (float)source->x;
    dest->y = (float)source->y;
    dest->width = (float)source->width;
    dest->height = (float)source->height;
}

static uint8_t texture_draw(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "draw expects a Vector2 position and a Color!");
    lt_Value tint = lt->pop(vm);
    LtVector2* position = vector2_data(vm, lt->pop(vm), "Expected a Vector2 position!");
    lt_Value instance = lt->pop(vm);
    Texture2D* texture = texture_data(vm, instance, "Expected a Texture!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    RayTextureCommand* payload = make_texture_command(vm, texture);
    payload->position.x = (float)position->x;
    payload->position.y = (float)position->y;
    RayCommand command = new_command(RAY_CMD_TEXTURE);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t texture_draw_rec(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "drawRec expects a source Rectangle, a Vector2 position, and a Color!");
    lt_Value tint = lt->pop(vm);
    LtVector2* position = vector2_data(vm, lt->pop(vm), "Expected a Vector2 position!");
    LtRectangle* source = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle source!");
    lt_Value instance = lt->pop(vm);
    Texture2D* texture = texture_data(vm, instance, "Expected a Texture!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    RayTextureCommand* payload = make_texture_command(vm, texture);
    copy_rectangle(&payload->source, source);
    payload->position.x = (float)position->x;
    payload->position.y = (float)position->y;
    RayCommand command = new_command(RAY_CMD_TEXTURE_REC);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t texture_draw_pro(lt_VM* vm, uint8_t argc)
{
    if (argc != 6) lt->runtime_error(vm, "drawPro expects a source Rectangle, a dest Rectangle, a Vector2 origin, a rotation, and a Color!");
    lt_Value tint = lt->pop(vm);
    lt_Value rotation_value = lt->pop(vm);
    LtVector2* origin = vector2_data(vm, lt->pop(vm), "Expected a Vector2 origin!");
    LtRectangle* dest = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle dest!");
    LtRectangle* source = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle source!");
    lt_Value instance = lt->pop(vm);
    Texture2D* texture = texture_data(vm, instance, "Expected a Texture!");
    expect_number(vm, rotation_value, "Expected a rotation number!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    RayTextureCommand* payload = make_texture_command(vm, texture);
    copy_rectangle(&payload->source, source);
    copy_rectangle(&payload->dest, dest);
    payload->origin.x = (float)origin->x;
    payload->origin.y = (float)origin->y;
    payload->rotation = (float)lt->get_number(rotation_value);
    RayCommand command = new_command(RAY_CMD_TEXTURE_PRO);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t texture_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    Texture2D* texture = texture_data(vm, lt->pop(vm), "Expected a Texture!");
    char text[48];
    snprintf(text, sizeof(text), "texture(%dx%d)", texture->width, texture->height);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t font_unload(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "unload expects no arguments!");
    lt_Value instance = lt->pop(vm);
    LtFontData* font = font_data(vm, instance, "Expected a Font!");
    if (resource_is_queued(vm, instance)) defer_unload(vm, font, RAY_RESOURCE_FONT);
    else unload_font_payload(font);
    return 0;
}

static uint8_t font_measure(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "measure expects text, a size, and a spacing!");
    lt_Value spacing_value = lt->pop(vm);
    lt_Value size_value = lt->pop(vm);
    lt_Value text_value = lt->pop(vm);
    LtFontData* font = font_data(vm, lt->pop(vm), "Expected a Font!");
    expect_string(vm, text_value, "Expected text to be string!");
    expect_number(vm, size_value, "Expected a font size number!");
    expect_number(vm, spacing_value, "Expected a spacing number!");
    if (font->font.texture.id == 0) lt->runtime_error(vm, "Ray font has been unloaded!");
    Vector2 measured = MeasureTextEx(font->font, lt->get_string(vm, text_value), (float)lt->get_number(size_value), (float)lt->get_number(spacing_value));
    lt->push(vm, make_vector2(vm, (double)measured.x, (double)measured.y));
    return 1;
}

static uint8_t font_draw(lt_VM* vm, uint8_t argc)
{
    if (argc != 6) lt->runtime_error(vm, "draw expects text, a Vector2 position, a size, a spacing, and a Color!");
    lt_Value tint = lt->pop(vm);
    lt_Value spacing_value = lt->pop(vm);
    lt_Value size_value = lt->pop(vm);
    lt_Value position_value = lt->pop(vm);
    lt_Value text_value = lt->pop(vm);
    lt_Value instance = lt->pop(vm);
    LtFontData* font = font_data(vm, instance, "Expected a Font!");
    expect_string(vm, text_value, "Expected text to be string!");
    expect_number(vm, size_value, "Expected a font size number!");
    expect_number(vm, spacing_value, "Expected a spacing number!");
    LtVector2* position = vector2_data(vm, position_value, "Expected a Vector2 position!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    if (font->font.texture.id == 0) lt->runtime_error(vm, "Ray font has been unloaded!");
    require_draw_target(vm);
    const char* source = lt->get_string(vm, text_value);
    char* copy = malloc(strlen(source) + 1);
    if (!copy) lt->runtime_error(vm, "Out of memory!");
    memcpy(copy, source, strlen(source) + 1);
    RayTextExCommand* payload = allocate_native_data(vm, sizeof(RayTextExCommand));
    memset(payload, 0, sizeof(RayTextExCommand));
    payload->font = font->font;
    payload->position.x = (float)position->x;
    payload->position.y = (float)position->y;
    payload->size = (float)lt->get_number(size_value);
    payload->spacing = (float)lt->get_number(spacing_value);
    RayCommand command = new_command(RAY_CMD_TEXT_EX);
    command.resource = instance;
    command.color = color;
    command.text = copy;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t font_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtFontData* font = font_data(vm, lt->pop(vm), "Expected a Font!");
    char text[48];
    snprintf(text, sizeof(text), "font(%d)", font->font.baseSize);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

/* ---------------- meshes, models, and render textures ---------------- */

static void attach_mesh(lt_VM* vm, Mesh source, uint8_t owned)
{
    lt_Value instance = construct(vm, mesh_class, 0, 0);
    LtMeshData* data = allocate_native_data(vm, sizeof(LtMeshData));
    data->mesh = source;
    data->owned = owned;
    lt->instance_set_native_data(vm, instance, mesh_class, data);
    lt->push(vm, instance);
}

static void attach_model(lt_VM* vm, Model source, uint8_t owned)
{
    lt_Value instance = construct(vm, model_class, 0, 0);
    LtModelData* data = allocate_native_data(vm, sizeof(LtModelData));
    data->model = source;
    data->owned = owned;
    lt->instance_set_native_data(vm, instance, model_class, data);
    lt->push(vm, instance);
}

static void attach_render_texture(lt_VM* vm, RenderTexture source)
{
    lt_Value instance = construct(vm, render_texture_class, 0, 0);
    RenderTexture* data = allocate_native_data(vm, sizeof(RenderTexture));
    *data = source;
    lt->instance_set_native_data(vm, instance, render_texture_class, data);
    lt->push(vm, instance);
}

static uint8_t native_gen_mesh_cube(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected width, height, and length for ray.genMeshCube!");
    lt_Value length_value = lt->pop(vm);
    lt_Value height_value = lt->pop(vm);
    lt_Value width_value = lt->pop(vm);
    expect_number(vm, width_value, "Expected a cube width number!");
    expect_number(vm, height_value, "Expected a cube height number!");
    expect_number(vm, length_value, "Expected a cube length number!");
    require_window(vm, "Expected ray.open before ray.genMeshCube!");
    attach_mesh(vm, GenMeshCube((float)lt->get_number(width_value), (float)lt->get_number(height_value), (float)lt->get_number(length_value)), 1);
    return 1;
}

static uint8_t native_gen_mesh_sphere(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a radius, rings, and slices for ray.genMeshSphere!");
    lt_Value slices_value = lt->pop(vm);
    lt_Value rings_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    expect_number(vm, radius_value, "Expected a sphere radius number!");
    expect_number(vm, rings_value, "Expected a sphere rings number!");
    expect_number(vm, slices_value, "Expected a sphere slices number!");
    require_window(vm, "Expected ray.open before ray.genMeshSphere!");
    attach_mesh(vm, GenMeshSphere((float)lt->get_number(radius_value), (int)lt->get_number(rings_value), (int)lt->get_number(slices_value)), 1);
    return 1;
}

static uint8_t native_gen_mesh_plane(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected a width, a length, resX, and resZ for ray.genMeshPlane!");
    lt_Value resz_value = lt->pop(vm);
    lt_Value resx_value = lt->pop(vm);
    lt_Value length_value = lt->pop(vm);
    lt_Value width_value = lt->pop(vm);
    expect_number(vm, width_value, "Expected a plane width number!");
    expect_number(vm, length_value, "Expected a plane length number!");
    expect_number(vm, resx_value, "Expected a plane resX number!");
    expect_number(vm, resz_value, "Expected a plane resZ number!");
    require_window(vm, "Expected ray.open before ray.genMeshPlane!");
    attach_mesh(vm, GenMeshPlane((float)lt->get_number(width_value), (float)lt->get_number(length_value),
        (int)lt->get_number(resx_value), (int)lt->get_number(resz_value)), 1);
    return 1;
}

static uint8_t native_gen_mesh_cylinder(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a radius, a height, and slices for ray.genMeshCylinder!");
    lt_Value slices_value = lt->pop(vm);
    lt_Value height_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    expect_number(vm, radius_value, "Expected a cylinder radius number!");
    expect_number(vm, height_value, "Expected a cylinder height number!");
    expect_number(vm, slices_value, "Expected a cylinder slices number!");
    require_window(vm, "Expected ray.open before ray.genMeshCylinder!");
    attach_mesh(vm, GenMeshCylinder((float)lt->get_number(radius_value), (float)lt->get_number(height_value), (int)lt->get_number(slices_value)), 1);
    return 1;
}

static uint8_t native_gen_mesh_torus(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected a radius, a size, radSeg, and sides for ray.genMeshTorus!");
    lt_Value sides_value = lt->pop(vm);
    lt_Value radseg_value = lt->pop(vm);
    lt_Value size_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    expect_number(vm, radius_value, "Expected a torus radius number!");
    expect_number(vm, size_value, "Expected a torus size number!");
    expect_number(vm, radseg_value, "Expected a torus radSeg number!");
    expect_number(vm, sides_value, "Expected a torus sides number!");
    require_window(vm, "Expected ray.open before ray.genMeshTorus!");
    attach_mesh(vm, GenMeshTorus((float)lt->get_number(radius_value), (float)lt->get_number(size_value),
        (int)lt->get_number(radseg_value), (int)lt->get_number(sides_value)), 1);
    return 1;
}

static uint8_t native_gen_mesh_knot(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected a radius, a size, radSeg, and sides for ray.genMeshKnot!");
    lt_Value sides_value = lt->pop(vm);
    lt_Value radseg_value = lt->pop(vm);
    lt_Value size_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    expect_number(vm, radius_value, "Expected a knot radius number!");
    expect_number(vm, size_value, "Expected a knot size number!");
    expect_number(vm, radseg_value, "Expected a knot radSeg number!");
    expect_number(vm, sides_value, "Expected a knot sides number!");
    require_window(vm, "Expected ray.open before ray.genMeshKnot!");
    attach_mesh(vm, GenMeshKnot((float)lt->get_number(radius_value), (float)lt->get_number(size_value),
        (int)lt->get_number(radseg_value), (int)lt->get_number(sides_value)), 1);
    return 1;
}

static uint8_t native_load_model(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a path for ray.loadModel!");
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected ray model path to be string!");
    require_window(vm, "Expected ray.open before ray.loadModel!");
    Model model = LoadModel(lt->get_string(vm, path));
    if (!IsModelValid(model)) lt->runtime_error(vm, "Failed to load ray model!");
    attach_model(vm, model, 1);
    return 1;
}

static uint8_t native_model_from_mesh(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a Mesh for ray.modelFromMesh!");
    LtMeshData* mesh = mesh_data(vm, lt->pop(vm), "Expected a Mesh for ray.modelFromMesh!");
    require_window(vm, "Expected ray.open before ray.modelFromMesh!");
    if (mesh->mesh.vertexCount <= 0) lt->runtime_error(vm, "Ray mesh has been unloaded!");
    if (!mesh->owned) lt->runtime_error(vm, "Ray mesh already belongs to a model!");
    Model model = LoadModelFromMesh(mesh->mesh);
    if (!IsModelValid(model)) lt->runtime_error(vm, "Failed to create a ray model!");
    /* The model frees the mesh now, so the instance must stop owning it. */
    mesh->owned = 0;
    attach_model(vm, model, 1);
    return 1;
}

static uint8_t native_load_render_texture(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected width and height for ray.loadRenderTexture!");
    lt_Value height_value = lt->pop(vm);
    lt_Value width_value = lt->pop(vm);
    expect_number(vm, width_value, "Expected a render texture width number!");
    expect_number(vm, height_value, "Expected a render texture height number!");
    int width = (int)lt->get_number(width_value);
    int height = (int)lt->get_number(height_value);
    if (width <= 0 || height <= 0) lt->runtime_error(vm, "Expected positive ray render texture dimensions!");
    require_window(vm, "Expected ray.open before ray.loadRenderTexture!");
    RenderTexture target = LoadRenderTexture(width, height);
    if (!IsRenderTextureValid(target)) lt->runtime_error(vm, "Failed to create a ray render texture!");
    attach_render_texture(vm, target);
    return 1;
}

static uint8_t mesh_get_vertex_count(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "vertexCount getter expects no arguments!");
    LtMeshData* mesh = mesh_data(vm, lt->pop(vm), "Expected a Mesh!");
    lt->push(vm, lt->make_number((double)mesh->mesh.vertexCount));
    return 1;
}

static uint8_t mesh_get_triangle_count(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "triangleCount getter expects no arguments!");
    LtMeshData* mesh = mesh_data(vm, lt->pop(vm), "Expected a Mesh!");
    lt->push(vm, lt->make_number((double)mesh->mesh.triangleCount));
    return 1;
}

static uint8_t mesh_unload(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "unload expects no arguments!");
    lt_Value instance = lt->pop(vm);
    LtMeshData* mesh = mesh_data(vm, instance, "Expected a Mesh!");
    if (resource_is_queued(vm, instance)) defer_unload(vm, mesh, RAY_RESOURCE_MESH);
    else unload_mesh_payload(mesh);
    return 0;
}

static uint8_t mesh_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtMeshData* mesh = mesh_data(vm, lt->pop(vm), "Expected a Mesh!");
    char text[64];
    snprintf(text, sizeof(text), "mesh(%d vertices, %d triangles)", mesh->mesh.vertexCount, mesh->mesh.triangleCount);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t model_get_mesh_count(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "meshCount getter expects no arguments!");
    LtModelData* model = model_data(vm, lt->pop(vm), "Expected a Model!");
    lt->push(vm, lt->make_number((double)model->model.meshCount));
    return 1;
}

static uint8_t model_get_material_count(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "materialCount getter expects no arguments!");
    LtModelData* model = model_data(vm, lt->pop(vm), "Expected a Model!");
    lt->push(vm, lt->make_number((double)model->model.materialCount));
    return 1;
}

static void queue_model_draw(lt_VM* vm, lt_Value instance, LtModelData* model, Vector3 position,
    Vector3 axis, float angle, Vector3 scale, Color color, uint8_t wires)
{
    if (model->model.meshCount <= 0) lt->runtime_error(vm, "Ray model has been unloaded!");
    require_draw_target(vm);
    RayModelCommand* payload = allocate_native_data(vm, sizeof(RayModelCommand));
    memset(payload, 0, sizeof(RayModelCommand));
    payload->model = model->model;
    payload->position = position;
    payload->rotationAxis = axis;
    payload->angle = angle;
    payload->scale = scale;
    payload->wires = wires;
    RayCommand command = new_command(RAY_CMD_MODEL);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
}

/* DrawModel is DrawModelEx with an upward axis, no rotation, and a uniform
   scale, so every draw goes through the extended path. */
static void queue_model_draw_simple(lt_VM* vm, uint8_t argc, uint8_t wires)
{
    if (argc != 4) lt->runtime_error(vm, wires
        ? "Expected a Vector3 position, a scale, and a Color for ray model drawWires!"
        : "Expected a Vector3 position, a scale, and a Color for ray model draw!");
    lt_Value tint = lt->pop(vm);
    lt_Value scale_value = lt->pop(vm);
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position!");
    lt_Value instance = lt->pop(vm);
    LtModelData* model = model_data(vm, instance, "Expected a Model!");
    expect_number(vm, scale_value, "Expected a model scale number!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    double scale = lt->get_number(scale_value);
    Vector3 axis = { 0.0f, 1.0f, 0.0f };
    Vector3 uniform = { (float)scale, (float)scale, (float)scale };
    queue_model_draw(vm, instance, model, position, axis, 0.0f, uniform, color, wires);
}

static void queue_model_draw_ex(lt_VM* vm, uint8_t argc, uint8_t wires)
{
    if (argc != 6) lt->runtime_error(vm, wires
        ? "Expected a Vector3 position, a Vector3 axis, an angle, a Vector3 scale, and a Color for ray model drawWiresEx!"
        : "Expected a Vector3 position, a Vector3 axis, an angle, a Vector3 scale, and a Color for ray model drawEx!");
    lt_Value tint = lt->pop(vm);
    Vector3 scale = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 scale!");
    lt_Value angle_value = lt->pop(vm);
    Vector3 axis = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 rotation axis!");
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position!");
    lt_Value instance = lt->pop(vm);
    LtModelData* model = model_data(vm, instance, "Expected a Model!");
    expect_number(vm, angle_value, "Expected a model rotation angle number!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    queue_model_draw(vm, instance, model, position, axis, (float)lt->get_number(angle_value), scale, color, wires);
}

static uint8_t model_draw(lt_VM* vm, uint8_t argc)
{
    queue_model_draw_simple(vm, argc, 0);
    return 0;
}

static uint8_t model_draw_wires(lt_VM* vm, uint8_t argc)
{
    queue_model_draw_simple(vm, argc, 1);
    return 0;
}

static uint8_t model_draw_ex(lt_VM* vm, uint8_t argc)
{
    queue_model_draw_ex(vm, argc, 0);
    return 0;
}

static uint8_t model_draw_wires_ex(lt_VM* vm, uint8_t argc)
{
    queue_model_draw_ex(vm, argc, 1);
    return 0;
}

static uint8_t model_unload(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "unload expects no arguments!");
    lt_Value instance = lt->pop(vm);
    LtModelData* model = model_data(vm, instance, "Expected a Model!");
    if (resource_is_queued(vm, instance)) defer_unload(vm, model, RAY_RESOURCE_MODEL);
    else unload_model_payload(model);
    return 0;
}

static uint8_t model_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    LtModelData* model = model_data(vm, lt->pop(vm), "Expected a Model!");
    char text[64];
    snprintf(text, sizeof(text), "model(%d meshes, %d materials)", model->model.meshCount, model->model.materialCount);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t render_texture_get_width(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "width getter expects no arguments!");
    RenderTexture* target = render_texture_data(vm, lt->pop(vm), "Expected a RenderTexture!");
    lt->push(vm, lt->make_number((double)target->texture.width));
    return 1;
}

static uint8_t render_texture_get_height(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "height getter expects no arguments!");
    RenderTexture* target = render_texture_data(vm, lt->pop(vm), "Expected a RenderTexture!");
    lt->push(vm, lt->make_number((double)target->texture.height));
    return 1;
}

/* Render textures are stored bottom-up, so drawing one flips the source rect;
   that is why these are separate from Texture draws rather than the same code. */
static RayTextureCommand* make_render_texture_command(lt_VM* vm, RenderTexture* target)
{
    if (target->id == 0) lt->runtime_error(vm, "Ray render texture has been unloaded!");
    require_draw_target(vm);
    RayTextureCommand* payload = allocate_native_data(vm, sizeof(RayTextureCommand));
    memset(payload, 0, sizeof(RayTextureCommand));
    payload->texture = target->texture;
    payload->source.width = (float)target->texture.width;
    payload->source.height = (float)-target->texture.height;
    return payload;
}

static uint8_t render_texture_draw(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "draw expects a Vector2 position and a Color!");
    lt_Value tint = lt->pop(vm);
    LtVector2* position = vector2_data(vm, lt->pop(vm), "Expected a Vector2 position!");
    lt_Value instance = lt->pop(vm);
    RenderTexture* target = render_texture_data(vm, instance, "Expected a RenderTexture!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    RayTextureCommand* payload = make_render_texture_command(vm, target);
    payload->position.x = (float)position->x;
    payload->position.y = (float)position->y;
    RayCommand command = new_command(RAY_CMD_TEXTURE_REC);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t render_texture_draw_pro(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "drawPro expects a dest Rectangle, a Vector2 origin, a rotation, and a Color!");
    lt_Value tint = lt->pop(vm);
    lt_Value rotation_value = lt->pop(vm);
    LtVector2* origin = vector2_data(vm, lt->pop(vm), "Expected a Vector2 origin!");
    LtRectangle* dest = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle dest!");
    lt_Value instance = lt->pop(vm);
    RenderTexture* target = render_texture_data(vm, instance, "Expected a RenderTexture!");
    expect_number(vm, rotation_value, "Expected a rotation number!");
    Color color = expect_color(vm, tint, "Expected a Color tint!");
    RayTextureCommand* payload = make_render_texture_command(vm, target);
    copy_rectangle(&payload->dest, dest);
    payload->origin.x = (float)origin->x;
    payload->origin.y = (float)origin->y;
    payload->rotation = (float)lt->get_number(rotation_value);
    RayCommand command = new_command(RAY_CMD_TEXTURE_PRO);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t render_texture_image(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "image expects no arguments!");
    RenderTexture* target = render_texture_data(vm, lt->pop(vm), "Expected a RenderTexture!");
    if (target->id == 0) lt->runtime_error(vm, "Ray render texture has been unloaded!");
    require_window(vm, "Expected ray.open before RenderTexture:image!");
    Image source = LoadImageFromTexture(target->texture);
    if (!IsImageValid(source)) lt->runtime_error(vm, "Failed to read the ray render texture!");
    attach_image(vm, source);
    return 1;
}

static uint8_t render_texture_unload(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "unload expects no arguments!");
    lt_Value instance = lt->pop(vm);
    RenderTexture* target = render_texture_data(vm, instance, "Expected a RenderTexture!");
    if (resource_is_queued(vm, instance)) defer_unload(vm, target, RAY_RESOURCE_RENDER_TEXTURE);
    else unload_render_texture_payload(target);
    return 0;
}

static uint8_t render_texture_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    RenderTexture* target = render_texture_data(vm, lt->pop(vm), "Expected a RenderTexture!");
    char text[48];
    snprintf(text, sizeof(text), "renderTexture(%dx%d)", target->texture.width, target->texture.height);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

/* ---------------- 2D shapes ---------------- */

static RayShapeCommand* make_shape_command(lt_VM* vm, RayShapeKind shape)
{
    require_draw_target(vm);
    RayShapeCommand* payload = allocate_native_data(vm, sizeof(RayShapeCommand));
    memset(payload, 0, sizeof(RayShapeCommand));
    payload->shape = shape;
    return payload;
}

static void push_shape_command(RayShapeCommand* payload, Color color)
{
    RayCommand command = new_command(RAY_CMD_SHAPE);
    command.color = color;
    command.payload = payload;
    push_command(command);
}

static void set_point(Vector2* point, LtVector2* source)
{
    point->x = (float)source->x;
    point->y = (float)source->y;
}

static uint8_t native_rect_lines(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Rectangle and a Color for ray.rectLines!");
    lt_Value color_value = lt->pop(vm);
    LtRectangle* bounds = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle for ray.rectLines!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.rectLines!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_RECT_LINES);
    copy_rectangle(&payload->bounds, bounds);
    payload->thickness = 1.0f;
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_circle(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector2 center, a radius, and a Color for ray.circle!");
    lt_Value color_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    LtVector2* center = vector2_data(vm, lt->pop(vm), "Expected a Vector2 center for ray.circle!");
    expect_number(vm, radius_value, "Expected a circle radius number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.circle!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_CIRCLE);
    set_point(&payload->points[0], center);
    payload->radius = (float)lt->get_number(radius_value);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_circle_lines(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector2 center, a radius, and a Color for ray.circleLines!");
    lt_Value color_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    LtVector2* center = vector2_data(vm, lt->pop(vm), "Expected a Vector2 center for ray.circleLines!");
    expect_number(vm, radius_value, "Expected a circle radius number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.circleLines!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_CIRCLE_LINES);
    set_point(&payload->points[0], center);
    payload->radius = (float)lt->get_number(radius_value);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_line(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected two Vector2 points and a Color for ray.line!");
    lt_Value color_value = lt->pop(vm);
    LtVector2* end = vector2_data(vm, lt->pop(vm), "Expected a Vector2 end point for ray.line!");
    LtVector2* start = vector2_data(vm, lt->pop(vm), "Expected a Vector2 start point for ray.line!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.line!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_LINE);
    set_point(&payload->points[0], start);
    set_point(&payload->points[1], end);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_line_ex(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected two Vector2 points, a thickness, and a Color for ray.lineEx!");
    lt_Value color_value = lt->pop(vm);
    lt_Value thickness_value = lt->pop(vm);
    LtVector2* end = vector2_data(vm, lt->pop(vm), "Expected a Vector2 end point for ray.lineEx!");
    LtVector2* start = vector2_data(vm, lt->pop(vm), "Expected a Vector2 start point for ray.lineEx!");
    expect_number(vm, thickness_value, "Expected a line thickness number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.lineEx!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_LINE_EX);
    set_point(&payload->points[0], start);
    set_point(&payload->points[1], end);
    payload->thickness = (float)lt->get_number(thickness_value);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_triangle(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected three Vector2 points and a Color for ray.triangle!");
    lt_Value color_value = lt->pop(vm);
    LtVector2* third = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point for ray.triangle!");
    LtVector2* second = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point for ray.triangle!");
    LtVector2* first = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point for ray.triangle!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.triangle!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_TRIANGLE);
    set_point(&payload->points[0], first);
    set_point(&payload->points[1], second);
    set_point(&payload->points[2], third);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_triangle_lines(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected three Vector2 points and a Color for ray.triangleLines!");
    lt_Value color_value = lt->pop(vm);
    LtVector2* third = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point for ray.triangleLines!");
    LtVector2* second = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point for ray.triangleLines!");
    LtVector2* first = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point for ray.triangleLines!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.triangleLines!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_TRIANGLE_LINES);
    set_point(&payload->points[0], first);
    set_point(&payload->points[1], second);
    set_point(&payload->points[2], third);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_polygon(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "Expected a Vector2 center, sides, a radius, a rotation, and a Color for ray.polygon!");
    lt_Value color_value = lt->pop(vm);
    lt_Value rotation_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    lt_Value sides_value = lt->pop(vm);
    LtVector2* center = vector2_data(vm, lt->pop(vm), "Expected a Vector2 center for ray.polygon!");
    expect_number(vm, sides_value, "Expected a polygon sides number!");
    expect_number(vm, radius_value, "Expected a polygon radius number!");
    expect_number(vm, rotation_value, "Expected a polygon rotation number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.polygon!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_POLYGON);
    set_point(&payload->points[0], center);
    payload->sides = (int)lt->get_number(sides_value);
    payload->radius = (float)lt->get_number(radius_value);
    payload->rotation = (float)lt->get_number(rotation_value);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_polygon_lines(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "Expected a Vector2 center, sides, a radius, a rotation, and a Color for ray.polygonLines!");
    lt_Value color_value = lt->pop(vm);
    lt_Value rotation_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    lt_Value sides_value = lt->pop(vm);
    LtVector2* center = vector2_data(vm, lt->pop(vm), "Expected a Vector2 center for ray.polygonLines!");
    expect_number(vm, sides_value, "Expected a polygon sides number!");
    expect_number(vm, radius_value, "Expected a polygon radius number!");
    expect_number(vm, rotation_value, "Expected a polygon rotation number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.polygonLines!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_POLYGON_LINES);
    set_point(&payload->points[0], center);
    payload->sides = (int)lt->get_number(sides_value);
    payload->radius = (float)lt->get_number(radius_value);
    payload->rotation = (float)lt->get_number(rotation_value);
    push_shape_command(payload, color);
    return 0;
}

static uint8_t native_ring(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected a Vector2 center, an inner radius, an outer radius, and a Color for ray.ring!");
    lt_Value color_value = lt->pop(vm);
    lt_Value outer_value = lt->pop(vm);
    lt_Value inner_value = lt->pop(vm);
    LtVector2* center = vector2_data(vm, lt->pop(vm), "Expected a Vector2 center for ray.ring!");
    expect_number(vm, inner_value, "Expected an inner radius number!");
    expect_number(vm, outer_value, "Expected an outer radius number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.ring!");
    RayShapeCommand* payload = make_shape_command(vm, RAY_SHAPE_RING);
    set_point(&payload->points[0], center);
    payload->radius = (float)lt->get_number(inner_value);
    payload->radius2 = (float)lt->get_number(outer_value);
    push_shape_command(payload, color);
    return 0;
}

/* ---------------- 3D shapes ---------------- */

static RayShape3DCommand* make_shape3d_command(lt_VM* vm, RayShape3DKind shape)
{
    require_draw_target(vm);
    RayShape3DCommand* payload = allocate_native_data(vm, sizeof(RayShape3DCommand));
    memset(payload, 0, sizeof(RayShape3DCommand));
    payload->shape = shape;
    return payload;
}

static void push_shape3d_command(RayShape3DCommand* payload, Color color)
{
    RayCommand command = new_command(RAY_CMD_SHAPE3D);
    command.color = color;
    command.payload = payload;
    push_command(command);
}

static uint8_t native_cube(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector3 position, a Vector3 size, and a Color for ray.cube!");
    lt_Value color_value = lt->pop(vm);
    Vector3 size = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 size for ray.cube!");
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.cube!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.cube!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_CUBE);
    payload->points[0] = position;
    payload->size = size;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_cube_wires(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector3 position, a Vector3 size, and a Color for ray.cubeWires!");
    lt_Value color_value = lt->pop(vm);
    Vector3 size = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 size for ray.cubeWires!");
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.cubeWires!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.cubeWires!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_CUBE_WIRES);
    payload->points[0] = position;
    payload->size = size;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_sphere(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector3 center, a radius, and a Color for ray.sphere!");
    lt_Value color_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    Vector3 center = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 center for ray.sphere!");
    expect_number(vm, radius_value, "Expected a sphere radius number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.sphere!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_SPHERE);
    payload->points[0] = center;
    payload->radius = (float)lt->get_number(radius_value);
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_sphere_wires(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "Expected a Vector3 center, a radius, rings, slices, and a Color for ray.sphereWires!");
    lt_Value color_value = lt->pop(vm);
    lt_Value slices_value = lt->pop(vm);
    lt_Value rings_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    Vector3 center = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 center for ray.sphereWires!");
    expect_number(vm, radius_value, "Expected a sphere radius number!");
    expect_number(vm, rings_value, "Expected a sphere rings number!");
    expect_number(vm, slices_value, "Expected a sphere slices number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.sphereWires!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_SPHERE_WIRES);
    payload->points[0] = center;
    payload->radius = (float)lt->get_number(radius_value);
    payload->rings = (int)lt->get_number(rings_value);
    payload->slices = (int)lt->get_number(slices_value);
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_cylinder(lt_VM* vm, uint8_t argc)
{
    if (argc != 6) lt->runtime_error(vm, "Expected a Vector3 position, a top radius, a bottom radius, a height, sides, and a Color for ray.cylinder!");
    lt_Value color_value = lt->pop(vm);
    lt_Value sides_value = lt->pop(vm);
    lt_Value height_value = lt->pop(vm);
    lt_Value bottom_value = lt->pop(vm);
    lt_Value top_value = lt->pop(vm);
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.cylinder!");
    expect_number(vm, top_value, "Expected a cylinder top radius number!");
    expect_number(vm, bottom_value, "Expected a cylinder bottom radius number!");
    expect_number(vm, height_value, "Expected a cylinder height number!");
    expect_number(vm, sides_value, "Expected a cylinder sides number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.cylinder!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_CYLINDER);
    payload->points[0] = position;
    payload->radius = (float)lt->get_number(top_value);
    payload->radius2 = (float)lt->get_number(bottom_value);
    payload->height = (float)lt->get_number(height_value);
    payload->sides = (int)lt->get_number(sides_value);
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_cylinder_wires(lt_VM* vm, uint8_t argc)
{
    if (argc != 6) lt->runtime_error(vm, "Expected a Vector3 position, a top radius, a bottom radius, a height, sides, and a Color for ray.cylinderWires!");
    lt_Value color_value = lt->pop(vm);
    lt_Value sides_value = lt->pop(vm);
    lt_Value height_value = lt->pop(vm);
    lt_Value bottom_value = lt->pop(vm);
    lt_Value top_value = lt->pop(vm);
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.cylinderWires!");
    expect_number(vm, top_value, "Expected a cylinder top radius number!");
    expect_number(vm, bottom_value, "Expected a cylinder bottom radius number!");
    expect_number(vm, height_value, "Expected a cylinder height number!");
    expect_number(vm, sides_value, "Expected a cylinder sides number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.cylinderWires!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_CYLINDER_WIRES);
    payload->points[0] = position;
    payload->radius = (float)lt->get_number(top_value);
    payload->radius2 = (float)lt->get_number(bottom_value);
    payload->height = (float)lt->get_number(height_value);
    payload->sides = (int)lt->get_number(sides_value);
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_capsule(lt_VM* vm, uint8_t argc)
{
    if (argc != 6) lt->runtime_error(vm, "Expected two Vector3 points, a radius, rings, slices, and a Color for ray.capsule!");
    lt_Value color_value = lt->pop(vm);
    lt_Value slices_value = lt->pop(vm);
    lt_Value rings_value = lt->pop(vm);
    lt_Value radius_value = lt->pop(vm);
    Vector3 end = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 end point for ray.capsule!");
    Vector3 start = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 start point for ray.capsule!");
    expect_number(vm, radius_value, "Expected a capsule radius number!");
    expect_number(vm, rings_value, "Expected a capsule rings number!");
    expect_number(vm, slices_value, "Expected a capsule slices number!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.capsule!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_CAPSULE);
    payload->points[0] = start;
    payload->points[1] = end;
    payload->radius = (float)lt->get_number(radius_value);
    payload->rings = (int)lt->get_number(rings_value);
    payload->slices = (int)lt->get_number(slices_value);
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_grid(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected slices and a spacing for ray.grid!");
    lt_Value spacing_value = lt->pop(vm);
    lt_Value slices_value = lt->pop(vm);
    expect_number(vm, slices_value, "Expected a grid slices number!");
    expect_number(vm, spacing_value, "Expected a grid spacing number!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_GRID);
    payload->sides = (int)lt->get_number(slices_value);
    payload->thickness = (float)lt->get_number(spacing_value);
    push_shape3d_command(payload, GRAY);
    return 0;
}

static uint8_t native_line3d(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected two Vector3 points and a Color for ray.line3D!");
    lt_Value color_value = lt->pop(vm);
    Vector3 end = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 end point for ray.line3D!");
    Vector3 start = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 start point for ray.line3D!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.line3D!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_LINE);
    payload->points[0] = start;
    payload->points[1] = end;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_point3d(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Vector3 position and a Color for ray.point3D!");
    lt_Value color_value = lt->pop(vm);
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.point3D!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.point3D!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_POINT);
    payload->points[0] = position;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_triangle3d(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected three Vector3 points and a Color for ray.triangle3D!");
    lt_Value color_value = lt->pop(vm);
    Vector3 third = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 point for ray.triangle3D!");
    Vector3 second = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 point for ray.triangle3D!");
    Vector3 first = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 point for ray.triangle3D!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.triangle3D!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_TRIANGLE);
    payload->points[0] = first;
    payload->points[1] = second;
    payload->points[2] = third;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_plane(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector3 center, a Vector2 size, and a Color for ray.plane!");
    lt_Value color_value = lt->pop(vm);
    LtVector2* size = vector2_data(vm, lt->pop(vm), "Expected a Vector2 size for ray.plane!");
    Vector3 center = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 center for ray.plane!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.plane!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_PLANE);
    payload->points[0] = center;
    payload->size.x = (float)size->x;
    payload->size.z = (float)size->y;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_bounding_box(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected two Vector3 corners and a Color for ray.boundingBox!");
    lt_Value color_value = lt->pop(vm);
    Vector3 max = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 max corner for ray.boundingBox!");
    Vector3 min = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 min corner for ray.boundingBox!");
    Color color = expect_color(vm, color_value, "Expected a Color for ray.boundingBox!");
    RayShape3DCommand* payload = make_shape3d_command(vm, RAY_SHAPE3D_BOX);
    payload->points[0] = min;
    payload->points[1] = max;
    push_shape3d_command(payload, color);
    return 0;
}

static uint8_t native_billboard(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "Expected a Camera3D, a Texture, a Vector3 position, a scale, and a Color for ray.billboard!");
    lt_Value tint = lt->pop(vm);
    lt_Value scale_value = lt->pop(vm);
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.billboard!");
    lt_Value instance = lt->pop(vm);
    Texture2D* texture = texture_data(vm, instance, "Expected a Texture for ray.billboard!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D for ray.billboard!");
    expect_number(vm, scale_value, "Expected a billboard scale number!");
    Color color = expect_color(vm, tint, "Expected a Color tint for ray.billboard!");
    if (texture->id == 0) lt->runtime_error(vm, "Ray texture has been unloaded!");
    require_draw_target(vm);
    RayBillboardCommand* payload = allocate_native_data(vm, sizeof(RayBillboardCommand));
    memset(payload, 0, sizeof(RayBillboardCommand));
    payload->camera = *camera;
    payload->texture = *texture;
    payload->position = position;
    payload->scale = (float)lt->get_number(scale_value);
    RayCommand command = new_command(RAY_CMD_BILLBOARD);
    command.resource = instance;
    command.color = color;
    command.payload = payload;
    push_command(command);
    queue_resource(vm, instance);
    return 0;
}

static uint8_t native_begin_mode3d(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a Camera3D for ray.beginMode3D!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D for ray.beginMode3D!");
    require_draw_target(vm);
    Camera3D* payload = allocate_native_data(vm, sizeof(Camera3D));
    *payload = *camera;
    RayCommand command = new_command(RAY_CMD_BEGIN_3D);
    command.payload = payload;
    push_command(command);
    mode3d_depth++;
    return 0;
}

static uint8_t native_end_mode3d(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.endMode3D!");
    require_draw_target(vm);
    if (mode3d_depth == 0) lt->runtime_error(vm, "Expected ray.beginMode3D before ray.endMode3D!");
    mode3d_depth--;
    RayCommand command = new_command(RAY_CMD_END_3D);
    push_command(command);
    return 0;
}

static uint8_t native_begin_texture_mode(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a RenderTexture for ray.beginTextureMode!");
    lt_Value instance = lt->pop(vm);
    RenderTexture* target = render_texture_data(vm, instance, "Expected a RenderTexture for ray.beginTextureMode!");
    require_draw_target(vm);
    if (target->id == 0) lt->runtime_error(vm, "Ray render texture has been unloaded!");
    RenderTexture* payload = allocate_native_data(vm, sizeof(RenderTexture));
    *payload = *target;
    RayCommand command = new_command(RAY_CMD_BEGIN_TEXTURE);
    command.resource = instance;
    command.payload = payload;
    push_command(command);
    /* The framebuffer has to outlive the callback, like a queued draw's
       resource, so hold the instance until the frame is replayed. */
    queue_resource(vm, instance);
    texture_depth++;
    return 0;
}

static uint8_t native_end_texture_mode(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.endTextureMode!");
    require_draw_target(vm);
    if (texture_depth == 0) lt->runtime_error(vm, "Expected ray.beginTextureMode before ray.endTextureMode!");
    texture_depth--;
    RayCommand command = new_command(RAY_CMD_END_TEXTURE);
    push_command(command);
    return 0;
}

/* ---------------- Camera2D ---------------- */

static Vector2 read_vector2_field(lt_VM* vm, lt_Value table, const char* key, Vector2 fallback)
{
    lt_Value value = lt->table_get(vm, table, lt->make_string(vm, key));
    if (LT_IS_NULL(value)) return fallback;
    LtVector2* vector = vector2_data(vm, value, "Expected a Vector2 camera field!");
    Vector2 result;
    result.x = (float)vector->x;
    result.y = (float)vector->y;
    return result;
}

static uint8_t camera2d_constructor(lt_VM* vm, uint8_t argc)
{
    Vector2 zero = { 0.0f, 0.0f };
    Camera2D camera;
    camera.offset = zero;
    camera.target = zero;
    camera.rotation = 0.0f;
    camera.zoom = 1.0f;
    if (argc == 2)
    {
        lt_Value source = lt->pop(vm);
        if (!LT_IS_TABLE(source)) lt->runtime_error(vm, "Expected a table, or offset and target Vector2s for Camera2D!");
        camera.offset = read_vector2_field(vm, source, "offset", zero);
        camera.target = read_vector2_field(vm, source, "target", zero);
        camera.rotation = (float)table_field_number(vm, source, "rotation", 0, "Expected Camera2D rotation to be number!");
        camera.zoom = (float)table_field_number(vm, source, "zoom", 1, "Expected Camera2D zoom to be number!");
    }
    else if (argc == 3 || argc == 5)
    {
        float zoom = 1.0f;
        float rotation = 0.0f;
        if (argc == 5)
        {
            lt_Value zoom_value = lt->pop(vm);
            lt_Value rotation_value = lt->pop(vm);
            expect_number(vm, rotation_value, "Expected a Camera2D rotation number!");
            expect_number(vm, zoom_value, "Expected a Camera2D zoom number!");
            rotation = (float)lt->get_number(rotation_value);
            zoom = (float)lt->get_number(zoom_value);
        }
        LtVector2* target = vector2_data(vm, lt->pop(vm), "Expected a Vector2 target for Camera2D!");
        LtVector2* offset = vector2_data(vm, lt->pop(vm), "Expected a Vector2 offset for Camera2D!");
        camera.offset.x = (float)offset->x;
        camera.offset.y = (float)offset->y;
        camera.target.x = (float)target->x;
        camera.target.y = (float)target->y;
        camera.rotation = rotation;
        camera.zoom = zoom;
    }
    else if (argc != 1)
    {
        lt->runtime_error(vm, "Expected a table, or offset and target Vector2s for Camera2D!");
    }
    lt_Value instance = lt->pop(vm);
    Camera2D* data = allocate_native_data(vm, sizeof(Camera2D));
    *data = camera;
    lt->instance_set_native_data(vm, instance, camera2d_class, data);
    return 0;
}

static uint8_t camera2d_get_target(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "target getter expects no arguments!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D!");
    lt->push(vm, make_vector2(vm, camera->target.x, camera->target.y));
    return 1;
}

static uint8_t camera2d_set_target(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "target setter expects one value!");
    LtVector2* target = vector2_data(vm, lt->pop(vm), "Expected a Vector2 for target!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D!");
    camera->target.x = (float)target->x;
    camera->target.y = (float)target->y;
    return 0;
}

static uint8_t camera2d_get_offset(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "offset getter expects no arguments!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D!");
    lt->push(vm, make_vector2(vm, camera->offset.x, camera->offset.y));
    return 1;
}

static uint8_t camera2d_set_offset(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "offset setter expects one value!");
    LtVector2* offset = vector2_data(vm, lt->pop(vm), "Expected a Vector2 for offset!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D!");
    camera->offset.x = (float)offset->x;
    camera->offset.y = (float)offset->y;
    return 0;
}

static uint8_t camera2d_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D!");
    char text[128];
    snprintf(text, sizeof(text), "camera2D(offset (%g, %g), target (%g, %g), rotation %g, zoom %g)",
        camera->offset.x, camera->offset.y, camera->target.x, camera->target.y, camera->rotation, camera->zoom);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t native_begin_mode2d(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a Camera2D for ray.beginMode2D!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D for ray.beginMode2D!");
    require_draw_target(vm);
    Camera2D* payload = allocate_native_data(vm, sizeof(Camera2D));
    *payload = *camera;
    RayCommand command = new_command(RAY_CMD_BEGIN_2D);
    command.payload = payload;
    push_command(command);
    mode2d_depth++;
    return 0;
}

static uint8_t native_end_mode2d(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.endMode2D!");
    require_draw_target(vm);
    if (mode2d_depth == 0) lt->runtime_error(vm, "Expected ray.beginMode2D before ray.endMode2D!");
    mode2d_depth--;
    RayCommand command = new_command(RAY_CMD_END_2D);
    push_command(command);
    return 0;
}

/* ---------------- Camera3D, Ray, and ray collisions ---------------- */

static Vector3 read_vector3_field(lt_VM* vm, lt_Value table, const char* key, Vector3 fallback)
{
    lt_Value value = lt->table_get(vm, table, lt->make_string(vm, key));
    if (LT_IS_NULL(value)) return fallback;
    return expect_vector3(vm, value, "Expected a Vector3 camera field!");
}

/* Ray is built through its constructor like the other value types: the class
   installs its own payload, so setting it here as well would raise. */
static lt_Value make_ray(lt_VM* vm, Vector3 position, Vector3 direction)
{
    lt->push(vm, make_vector3(vm, position.x, position.y, position.z));
    lt->push(vm, make_vector3(vm, direction.x, direction.y, direction.z));
    uint16_t returns = lt->exec(vm, ray_class, 2);
    if (returns == 0) return LT_VALUE_NULL;
    return lt->pop(vm);
}

static void attach_ray_collision(lt_VM* vm, RayCollision source)
{
    lt_Value instance = construct(vm, ray_collision_class, 0, 0);
    RayCollision* data = allocate_native_data(vm, sizeof(RayCollision));
    *data = source;
    lt->instance_set_native_data(vm, instance, ray_collision_class, data);
    lt->push(vm, instance);
}

static uint8_t camera3d_constructor(lt_VM* vm, uint8_t argc)
{
    Camera3D camera;
    camera.position.x = 0.0f;
    camera.position.y = 0.0f;
    camera.position.z = 0.0f;
    camera.target = camera.position;
    camera.up.x = 0.0f;
    camera.up.y = 1.0f;
    camera.up.z = 0.0f;
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    if (argc == 2)
    {
        lt_Value source = lt->pop(vm);
        if (!LT_IS_TABLE(source)) lt->runtime_error(vm, "Expected a table, or position and target Vector3s for Camera3D!");
        camera.position = read_vector3_field(vm, source, "position", camera.position);
        camera.target = read_vector3_field(vm, source, "target", camera.target);
        camera.up = read_vector3_field(vm, source, "up", camera.up);
        camera.fovy = (float)table_field_number(vm, source, "fovy", 45, "Expected Camera3D fovy to be number!");
        camera.projection = (int)table_field_number(vm, source, "projection", CAMERA_PERSPECTIVE, "Expected Camera3D projection to be number!");
    }
    else if (argc >= 3 && argc <= 6)
    {
        float fovy = 45.0f;
        int projection = CAMERA_PERSPECTIVE;
        if (argc == 6)
        {
            lt_Value projection_value = lt->pop(vm);
            expect_number(vm, projection_value, "Expected a Camera3D projection number!");
            projection = (int)lt->get_number(projection_value);
        }
        if (argc >= 5)
        {
            lt_Value fovy_value = lt->pop(vm);
            expect_number(vm, fovy_value, "Expected a Camera3D fovy number!");
            fovy = (float)lt->get_number(fovy_value);
        }
        if (argc >= 4) camera.up = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 up for Camera3D!");
        camera.target = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 target for Camera3D!");
        camera.position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for Camera3D!");
        camera.fovy = fovy;
        camera.projection = projection;
    }
    else if (argc != 1)
    {
        lt->runtime_error(vm, "Expected a table, or position and target Vector3s for Camera3D!");
    }
    lt_Value instance = lt->pop(vm);
    Camera3D* data = allocate_native_data(vm, sizeof(Camera3D));
    *data = camera;
    lt->instance_set_native_data(vm, instance, camera3d_class, data);
    return 0;
}

static uint8_t camera3d_get_position(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "position getter expects no arguments!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    lt->push(vm, make_vector3(vm, camera->position.x, camera->position.y, camera->position.z));
    return 1;
}

static uint8_t camera3d_set_position(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "position setter expects one value!");
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 for position!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    camera->position = position;
    return 0;
}

static uint8_t camera3d_get_target(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "target getter expects no arguments!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    lt->push(vm, make_vector3(vm, camera->target.x, camera->target.y, camera->target.z));
    return 1;
}

static uint8_t camera3d_set_target(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "target setter expects one value!");
    Vector3 target = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 for target!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    camera->target = target;
    return 0;
}

static uint8_t camera3d_get_up(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "up getter expects no arguments!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    lt->push(vm, make_vector3(vm, camera->up.x, camera->up.y, camera->up.z));
    return 1;
}

static uint8_t camera3d_set_up(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "up setter expects one value!");
    Vector3 up = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 for up!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    camera->up = up;
    return 0;
}

static uint8_t camera3d_get_fovy(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "fovy getter expects no arguments!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    lt->push(vm, lt->make_number((double)camera->fovy));
    return 1;
}

static uint8_t camera3d_set_fovy(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "fovy setter expects one value!");
    lt_Value raw = lt->pop(vm);
    expect_number(vm, raw, "Expected a number for fovy!");
    double value = lt->get_number(raw);
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    camera->fovy = (float)value;
    return 0;
}

static uint8_t camera3d_get_projection(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "projection getter expects no arguments!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    lt->push(vm, lt->make_number((double)camera->projection));
    return 1;
}

static uint8_t camera3d_set_projection(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "projection setter expects one value!");
    lt_Value raw = lt->pop(vm);
    expect_number(vm, raw, "Expected a number for projection!");
    double value = lt->get_number(raw);
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    camera->projection = (int)value;
    return 0;
}

static uint8_t camera3d_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D!");
    char text[192];
    snprintf(text, sizeof(text), "camera3D(position (%g, %g, %g), target (%g, %g, %g), up (%g, %g, %g), fovy %g, projection %s)",
        camera->position.x, camera->position.y, camera->position.z,
        camera->target.x, camera->target.y, camera->target.z,
        camera->up.x, camera->up.y, camera->up.z,
        camera->fovy, camera->projection == CAMERA_ORTHOGRAPHIC ? "orthographic" : "perspective");
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t ray_constructor(lt_VM* vm, uint8_t argc)
{
    Vector3 zero = { 0.0f, 0.0f, 0.0f };
    Ray ray;
    ray.position = zero;
    ray.direction = zero;
    if (argc == 2)
    {
        lt_Value source = lt->pop(vm);
        if (!LT_IS_TABLE(source)) lt->runtime_error(vm, "Expected a table, or position and direction Vector3s for Ray!");
        ray.position = read_vector3_field(vm, source, "position", zero);
        ray.direction = read_vector3_field(vm, source, "direction", zero);
    }
    else if (argc == 3)
    {
        ray.direction = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 direction for Ray!");
        ray.position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for Ray!");
    }
    else if (argc != 1)
    {
        lt->runtime_error(vm, "Expected a table, or position and direction Vector3s for Ray!");
    }
    lt_Value instance = lt->pop(vm);
    Ray* data = allocate_native_data(vm, sizeof(Ray));
    *data = ray;
    lt->instance_set_native_data(vm, instance, ray_class, data);
    return 0;
}

static uint8_t ray_get_position(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "position getter expects no arguments!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray!");
    lt->push(vm, make_vector3(vm, ray->position.x, ray->position.y, ray->position.z));
    return 1;
}

static uint8_t ray_set_position(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "position setter expects one value!");
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 for position!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray!");
    ray->position = position;
    return 0;
}

static uint8_t ray_get_direction(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "direction getter expects no arguments!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray!");
    lt->push(vm, make_vector3(vm, ray->direction.x, ray->direction.y, ray->direction.z));
    return 1;
}

static uint8_t ray_set_direction(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "direction setter expects one value!");
    Vector3 direction = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 for direction!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray!");
    ray->direction = direction;
    return 0;
}

static uint8_t ray_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray!");
    char text[128];
    snprintf(text, sizeof(text), "ray((%g, %g, %g) -> (%g, %g, %g))",
        ray->position.x, ray->position.y, ray->position.z,
        ray->direction.x, ray->direction.y, ray->direction.z);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t ray_collision_get_hit(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "hit getter expects no arguments!");
    RayCollision* collision = ray_collision_data(vm, lt->pop(vm), "Expected a RayCollision!");
    lt->push(vm, make_boolean(collision->hit ? 1 : 0));
    return 1;
}

static uint8_t ray_collision_get_distance(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "distance getter expects no arguments!");
    RayCollision* collision = ray_collision_data(vm, lt->pop(vm), "Expected a RayCollision!");
    lt->push(vm, lt->make_number((double)collision->distance));
    return 1;
}

static uint8_t ray_collision_get_point(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "point getter expects no arguments!");
    RayCollision* collision = ray_collision_data(vm, lt->pop(vm), "Expected a RayCollision!");
    lt->push(vm, make_vector3(vm, collision->point.x, collision->point.y, collision->point.z));
    return 1;
}

static uint8_t ray_collision_get_normal(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "normal getter expects no arguments!");
    RayCollision* collision = ray_collision_data(vm, lt->pop(vm), "Expected a RayCollision!");
    lt->push(vm, make_vector3(vm, collision->normal.x, collision->normal.y, collision->normal.z));
    return 1;
}

static uint8_t ray_collision_to_string(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "toString expects no arguments!");
    RayCollision* collision = ray_collision_data(vm, lt->pop(vm), "Expected a RayCollision!");
    char text[160];
    snprintf(text, sizeof(text), "rayCollision(%s, %g, (%g, %g, %g), (%g, %g, %g))",
        collision->hit ? "hit" : "miss", (double)collision->distance,
        collision->point.x, collision->point.y, collision->point.z,
        collision->normal.x, collision->normal.y, collision->normal.z);
    lt->push(vm, lt->make_string(vm, text));
    return 1;
}

static uint8_t native_get_world_to_screen(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Vector3 position and a Camera3D for ray.getWorldToScreen!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D for ray.getWorldToScreen!");
    Vector3 position = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 position for ray.getWorldToScreen!");
    require_window(vm, "Expected ray.open before ray.getWorldToScreen!");
    Vector2 result = GetWorldToScreen(position, *camera);
    lt->push(vm, make_vector2(vm, (double)result.x, (double)result.y));
    return 1;
}

static uint8_t native_get_screen_to_world_ray(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Vector2 position and a Camera3D for ray.getScreenToWorldRay!");
    Camera3D* camera = camera3d_data(vm, lt->pop(vm), "Expected a Camera3D for ray.getScreenToWorldRay!");
    LtVector2* position = vector2_data(vm, lt->pop(vm), "Expected a Vector2 position for ray.getScreenToWorldRay!");
    require_window(vm, "Expected ray.open before ray.getScreenToWorldRay!");
    Vector2 point = { (float)position->x, (float)position->y };
    Ray result = GetScreenToWorldRay(point, *camera);
    lt->push(vm, make_ray(vm, result.position, result.direction));
    return 1;
}

static uint8_t native_raycast_sphere(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Ray, a Vector3 center, and a radius for ray.raycastSphere!");
    lt_Value radius_value = lt->pop(vm);
    Vector3 center = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 center for ray.raycastSphere!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray for ray.raycastSphere!");
    expect_number(vm, radius_value, "Expected a sphere radius number!");
    attach_ray_collision(vm, GetRayCollisionSphere(*ray, center, (float)lt->get_number(radius_value)));
    return 1;
}

static uint8_t native_raycast_box(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Ray and two Vector3 corners for ray.raycastBox!");
    Vector3 max = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 max corner for ray.raycastBox!");
    Vector3 min = expect_vector3(vm, lt->pop(vm), "Expected a Vector3 min corner for ray.raycastBox!");
    Ray* ray = ray_data(vm, lt->pop(vm), "Expected a Ray for ray.raycastBox!");
    BoundingBox box;
    box.min = min;
    box.max = max;
    attach_ray_collision(vm, GetRayCollisionBox(*ray, box));
    return 1;
}

/* ---------------- collision, colour, transforms, window control ---------------- */

static uint8_t native_check_collision_recs(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected two Rectangles for ray.checkCollisionRecs!");
    LtRectangle* second = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle for ray.checkCollisionRecs!");
    LtRectangle* first = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle for ray.checkCollisionRecs!");
    Rectangle a;
    Rectangle b;
    copy_rectangle(&a, first);
    copy_rectangle(&b, second);
    lt->push(vm, CheckCollisionRecs(a, b) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_check_collision_circles(lt_VM* vm, uint8_t argc)
{
    if (argc != 4) lt->runtime_error(vm, "Expected two Vector2 centers and two radii for ray.checkCollisionCircles!");
    lt_Value radius2_value = lt->pop(vm);
    LtVector2* center2 = vector2_data(vm, lt->pop(vm), "Expected a second Vector2 center!");
    lt_Value radius1_value = lt->pop(vm);
    LtVector2* center1 = vector2_data(vm, lt->pop(vm), "Expected a first Vector2 center!");
    expect_number(vm, radius1_value, "Expected a first radius number!");
    expect_number(vm, radius2_value, "Expected a second radius number!");
    Vector2 a = { (float)center1->x, (float)center1->y };
    Vector2 b = { (float)center2->x, (float)center2->y };
    uint8_t hit = CheckCollisionCircles(a, (float)lt->get_number(radius1_value), b, (float)lt->get_number(radius2_value));
    lt->push(vm, hit ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_check_collision_point_rec(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Vector2 point and a Rectangle for ray.checkCollisionPointRec!");
    LtRectangle* bounds = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle for ray.checkCollisionPointRec!");
    LtVector2* point = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point!");
    Rectangle rec;
    copy_rectangle(&rec, bounds);
    Vector2 position = { (float)point->x, (float)point->y };
    lt->push(vm, CheckCollisionPointRec(position, rec) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_check_collision_point_circle(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected a Vector2 point, a Vector2 center, and a radius for ray.checkCollisionPointCircle!");
    lt_Value radius_value = lt->pop(vm);
    LtVector2* center = vector2_data(vm, lt->pop(vm), "Expected a Vector2 center!");
    LtVector2* point = vector2_data(vm, lt->pop(vm), "Expected a Vector2 point!");
    expect_number(vm, radius_value, "Expected a radius number!");
    Vector2 position = { (float)point->x, (float)point->y };
    Vector2 origin = { (float)center->x, (float)center->y };
    uint8_t hit = CheckCollisionPointCircle(position, origin, (float)lt->get_number(radius_value));
    lt->push(vm, hit ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_get_collision_rec(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected two Rectangles for ray.getCollisionRec!");
    LtRectangle* second = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle for ray.getCollisionRec!");
    LtRectangle* first = rectangle_data(vm, lt->pop(vm), "Expected a Rectangle for ray.getCollisionRec!");
    Rectangle a;
    Rectangle b;
    copy_rectangle(&a, first);
    copy_rectangle(&b, second);
    Rectangle result = GetCollisionRec(a, b);
    lt->push(vm, make_rectangle(vm, result.x, result.y, result.width, result.height));
    return 1;
}

static uint8_t native_fade(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Color and an alpha for ray.fade!");
    lt_Value alpha_value = lt->pop(vm);
    lt_Value color_value = lt->pop(vm);
    expect_number(vm, alpha_value, "Expected an alpha number!");
    Color source = expect_color(vm, color_value, "Expected a Color for ray.fade!");
    Color result = Fade(source, (float)lt->get_number(alpha_value));
    lt->push(vm, make_color(vm, result.r, result.g, result.b, result.a));
    return 1;
}

static uint8_t native_color_lerp(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected two Colors and a factor for ray.colorLerp!");
    lt_Value factor_value = lt->pop(vm);
    lt_Value second_value = lt->pop(vm);
    lt_Value first_value = lt->pop(vm);
    expect_number(vm, factor_value, "Expected a color lerp factor number!");
    Color first = expect_color(vm, first_value, "Expected a Color for ray.colorLerp!");
    Color second = expect_color(vm, second_value, "Expected a Color for ray.colorLerp!");
    Color result = ColorLerp(first, second, (float)lt->get_number(factor_value));
    lt->push(vm, make_color(vm, result.r, result.g, result.b, result.a));
    return 1;
}

static uint8_t native_color_brightness(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Color and a factor for ray.colorBrightness!");
    lt_Value factor_value = lt->pop(vm);
    lt_Value color_value = lt->pop(vm);
    expect_number(vm, factor_value, "Expected a brightness factor number!");
    Color source = expect_color(vm, color_value, "Expected a Color for ray.colorBrightness!");
    Color result = ColorBrightness(source, (float)lt->get_number(factor_value));
    lt->push(vm, make_color(vm, result.r, result.g, result.b, result.a));
    return 1;
}

static uint8_t native_color_tint(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected two Colors for ray.colorTint!");
    lt_Value tint_value = lt->pop(vm);
    lt_Value color_value = lt->pop(vm);
    Color source = expect_color(vm, color_value, "Expected a Color for ray.colorTint!");
    Color tint = expect_color(vm, tint_value, "Expected a tint Color for ray.colorTint!");
    Color result = ColorTint(source, tint);
    lt->push(vm, make_color(vm, result.r, result.g, result.b, result.a));
    return 1;
}

static uint8_t native_color_from_hsv(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt->runtime_error(vm, "Expected hue, saturation, and value for ray.colorFromHSV!");
    lt_Value value_value = lt->pop(vm);
    lt_Value saturation_value = lt->pop(vm);
    lt_Value hue_value = lt->pop(vm);
    expect_number(vm, hue_value, "Expected a hue number!");
    expect_number(vm, saturation_value, "Expected a saturation number!");
    expect_number(vm, value_value, "Expected a value number!");
    Color result = ColorFromHSV((float)lt->get_number(hue_value), (float)lt->get_number(saturation_value), (float)lt->get_number(value_value));
    lt->push(vm, make_color(vm, result.r, result.g, result.b, result.a));
    return 1;
}

static uint8_t native_color_to_int(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a Color for ray.colorToInt!");
    Color source = expect_color(vm, lt->pop(vm), "Expected a Color for ray.colorToInt!");
    lt->push(vm, lt->make_number((double)ColorToInt(source)));
    return 1;
}

static uint8_t native_world_to_screen(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Vector2 position and a Camera2D for ray.worldToScreen!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D for ray.worldToScreen!");
    LtVector2* position = vector2_data(vm, lt->pop(vm), "Expected a Vector2 position!");
    Vector2 point = { (float)position->x, (float)position->y };
    Camera2D camera_copy = *camera;
    Vector2 result = GetWorldToScreen2D(point, camera_copy);
    lt->push(vm, make_vector2(vm, (double)result.x, (double)result.y));
    return 1;
}

static uint8_t native_screen_to_world(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Vector2 position and a Camera2D for ray.screenToWorld!");
    Camera2D* camera = camera2d_data(vm, lt->pop(vm), "Expected a Camera2D for ray.screenToWorld!");
    LtVector2* position = vector2_data(vm, lt->pop(vm), "Expected a Vector2 position!");
    Vector2 point = { (float)position->x, (float)position->y };
    Camera2D camera_copy = *camera;
    Vector2 result = GetScreenToWorld2D(point, camera_copy);
    lt->push(vm, make_vector2(vm, (double)result.x, (double)result.y));
    return 1;
}

static uint8_t native_set_window_title(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a title string for ray.setWindowTitle!");
    lt_Value title = lt->pop(vm);
    expect_string(vm, title, "Expected a window title string!");
    require_window(vm, "Expected ray.open before ray.setWindowTitle!");
    SetWindowTitle(lt->get_string(vm, title));
    return 0;
}

static uint8_t native_set_window_size(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected width and height for ray.setWindowSize!");
    lt_Value height_value = lt->pop(vm);
    lt_Value width_value = lt->pop(vm);
    expect_number(vm, width_value, "Expected a window width number!");
    expect_number(vm, height_value, "Expected a window height number!");
    require_window(vm, "Expected ray.open before ray.setWindowSize!");
    SetWindowSize((int)lt->get_number(width_value), (int)lt->get_number(height_value));
    return 0;
}

static uint8_t native_toggle_fullscreen(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.toggleFullscreen!");
    require_window(vm, "Expected ray.open before ray.toggleFullscreen!");
    ToggleFullscreen();
    return 0;
}

static uint8_t native_screenshot(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a path for ray.screenshot!");
    lt_Value path = lt->pop(vm);
    expect_string(vm, path, "Expected a screenshot path string!");
    require_window(vm, "Expected ray.open before ray.screenshot!");
    require_draw_target(vm);
    /* The frame is replayed after the callback returns, so the capture has to
       be queued too; taking it here would read a stale back buffer. */
    const char* source = lt->get_string(vm, path);
    char* copy = malloc(strlen(source) + 1);
    if (!copy) lt->runtime_error(vm, "Out of memory!");
    memcpy(copy, source, strlen(source) + 1);
    RayCommand command = new_command(RAY_CMD_SCREENSHOT);
    command.text = copy;
    push_command(command);
    return 0;
}

static uint8_t native_open(lt_VM* vm, uint8_t argc)
{
    if (argc != 3 && argc != 4) lt->runtime_error(vm, "Expected width, height, title, and optional window options for ray.open!");
    lt_Value options = argc == 4 ? lt->pop(vm) : LT_VALUE_NULL;
    lt_Value title = lt->pop(vm);
    lt_Value height = lt->pop(vm);
    lt_Value width = lt->pop(vm);
    expect_number(vm, width, "Expected ray window width to be number!");
    expect_number(vm, height, "Expected ray window height to be number!");
    expect_string(vm, title, "Expected ray window title to be string!");
    if (!LT_IS_NULL(options) && !LT_IS_TABLE(options)) lt->runtime_error(vm, "Expected a table of ray window options!");
    if (window_open) lt->runtime_error(vm, "Ray window is already open!");
    unsigned int flags = 0;
    if (LT_IS_TABLE(options))
    {
        if (table_field_bool(vm, options, "hidden", 0)) flags |= FLAG_WINDOW_HIDDEN;
        if (table_field_bool(vm, options, "resizable", 0)) flags |= FLAG_WINDOW_RESIZABLE;
        if (table_field_bool(vm, options, "vsync", 0)) flags |= FLAG_VSYNC_HINT;
        if (table_field_bool(vm, options, "fullscreen", 0)) flags |= FLAG_FULLSCREEN_MODE;
        if (table_field_bool(vm, options, "undecorated", 0)) flags |= FLAG_WINDOW_UNDECORATED;
        if (table_field_bool(vm, options, "alwaysRun", 0)) flags |= FLAG_WINDOW_ALWAYS_RUN;
    }
    SetConfigFlags(flags);
    InitWindow((int)lt->get_number(width), (int)lt->get_number(height), lt->get_string(vm, title));
    if (!IsWindowReady()) lt->runtime_error(vm, "Failed to initialize the ray window!");
    window_open = 1;
    started = 0;
    clear_commands(vm);
    return 0;
}

static uint8_t native_trace_log(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a log level for ray.traceLog!");
    lt_Value level = lt->pop(vm);
    expect_number(vm, level, "Expected a ray log level number!");
    SetTraceLogLevel((int)lt->get_number(level));
    return 0;
}

static uint8_t native_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.close!");
    if (window_open)
    {
        CloseWindow();
        window_open = 0;
        clear_commands(vm);
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
        clear_commands(vm);
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
        clear_commands(vm);
        lt->push(vm, LT_VALUE_FALSE);
        return 1;
    }
    if (mode2d_depth != 0)
    {
        clear_commands(vm);
        lt->runtime_error(vm, "Expected ray.endMode2D before the frame ends!");
    }
    if (mode3d_depth != 0)
    {
        clear_commands(vm);
        lt->runtime_error(vm, "Expected ray.endMode3D before the frame ends!");
    }
    if (texture_depth != 0)
    {
        clear_commands(vm);
        lt->runtime_error(vm, "Expected ray.endTextureMode before the frame ends!");
    }
    BeginDrawing();
    /* The frame starts black; ray.clear queues a clear of its own, so a script
       that clears inside a render target clears that target, not the screen. */
    ClearBackground(BLACK);
    for (uint32_t i = 0; i < command_count; ++i)
    {
        RayCommand* command = &commands[i];
        switch (command->kind)
        {
        case RAY_CMD_RECT:
            DrawRectangle(command->x, command->y, command->w, command->h, command->color);
            break;
        case RAY_CMD_TEXT:
            DrawText(command->text, command->x, command->y, command->size, command->color);
            break;
        case RAY_CMD_TEXTURE: {
            RayTextureCommand* payload = command->payload;
            DrawTextureV(payload->texture, payload->position, command->color);
            break;
        }
        case RAY_CMD_TEXTURE_REC: {
            RayTextureCommand* payload = command->payload;
            DrawTextureRec(payload->texture, payload->source, payload->position, command->color);
            break;
        }
        case RAY_CMD_TEXTURE_PRO: {
            RayTextureCommand* payload = command->payload;
            DrawTexturePro(payload->texture, payload->source, payload->dest,
                payload->origin, payload->rotation, command->color);
            break;
        }
        case RAY_CMD_TEXT_EX: {
            RayTextExCommand* payload = command->payload;
            DrawTextEx(payload->font, command->text, payload->position,
                payload->size, payload->spacing, command->color);
            break;
        }
        case RAY_CMD_SHAPE: {
            RayShapeCommand* shape = command->payload;
            switch (shape->shape)
            {
            case RAY_SHAPE_RECT_LINES:
                DrawRectangleLinesEx(shape->bounds, shape->thickness, command->color);
                break;
            case RAY_SHAPE_CIRCLE:
                DrawCircleV(shape->points[0], shape->radius, command->color);
                break;
            case RAY_SHAPE_CIRCLE_LINES:
                DrawCircleLinesV(shape->points[0], shape->radius, command->color);
                break;
            case RAY_SHAPE_LINE:
                DrawLineV(shape->points[0], shape->points[1], command->color);
                break;
            case RAY_SHAPE_LINE_EX:
                DrawLineEx(shape->points[0], shape->points[1], shape->thickness, command->color);
                break;
            case RAY_SHAPE_TRIANGLE:
                DrawTriangle(shape->points[0], shape->points[1], shape->points[2], command->color);
                break;
            case RAY_SHAPE_TRIANGLE_LINES:
                DrawTriangleLines(shape->points[0], shape->points[1], shape->points[2], command->color);
                break;
            case RAY_SHAPE_POLYGON:
                DrawPoly(shape->points[0], shape->sides, shape->radius, shape->rotation, command->color);
                break;
            case RAY_SHAPE_POLYGON_LINES:
                DrawPolyLines(shape->points[0], shape->sides, shape->radius, shape->rotation, command->color);
                break;
            case RAY_SHAPE_RING:
                DrawRing(shape->points[0], shape->radius, shape->radius2, 0.0f, 360.0f, 64, command->color);
                break;
            }
            break;
        }
        case RAY_CMD_BEGIN_2D:
            BeginMode2D(*(Camera2D*)command->payload);
            break;
        case RAY_CMD_END_2D:
            EndMode2D();
            break;
        case RAY_CMD_SHAPE3D: {
            RayShape3DCommand* shape = command->payload;
            switch (shape->shape)
            {
            case RAY_SHAPE3D_CUBE:
                DrawCubeV(shape->points[0], shape->size, command->color);
                break;
            case RAY_SHAPE3D_CUBE_WIRES:
                DrawCubeWiresV(shape->points[0], shape->size, command->color);
                break;
            case RAY_SHAPE3D_SPHERE:
                DrawSphere(shape->points[0], shape->radius, command->color);
                break;
            case RAY_SHAPE3D_SPHERE_WIRES:
                DrawSphereWires(shape->points[0], shape->radius, shape->rings, shape->slices, command->color);
                break;
            case RAY_SHAPE3D_CYLINDER:
                DrawCylinder(shape->points[0], shape->radius, shape->radius2, shape->height, shape->sides, command->color);
                break;
            case RAY_SHAPE3D_CYLINDER_WIRES:
                DrawCylinderWires(shape->points[0], shape->radius, shape->radius2, shape->height, shape->sides, command->color);
                break;
            case RAY_SHAPE3D_CAPSULE:
                DrawCapsule(shape->points[0], shape->points[1], shape->radius, shape->rings, shape->slices, command->color);
                break;
            case RAY_SHAPE3D_GRID:
                DrawGrid(shape->sides, shape->thickness);
                break;
            case RAY_SHAPE3D_LINE:
                DrawLine3D(shape->points[0], shape->points[1], command->color);
                break;
            case RAY_SHAPE3D_POINT:
                DrawPoint3D(shape->points[0], command->color);
                break;
            case RAY_SHAPE3D_TRIANGLE:
                DrawTriangle3D(shape->points[0], shape->points[1], shape->points[2], command->color);
                break;
            case RAY_SHAPE3D_PLANE: {
                Vector2 size = { shape->size.x, shape->size.z };
                DrawPlane(shape->points[0], size, command->color);
                break;
            }
            case RAY_SHAPE3D_BOX: {
                BoundingBox box;
                box.min = shape->points[0];
                box.max = shape->points[1];
                DrawBoundingBox(box, command->color);
                break;
            }
            }
            break;
        }
        case RAY_CMD_CLEAR:
            ClearBackground(command->color);
            break;
        case RAY_CMD_SCREENSHOT:
            TakeScreenshot(command->text);
            break;
        case RAY_CMD_BEGIN_3D:
            BeginMode3D(*(Camera3D*)command->payload);
            break;
        case RAY_CMD_END_3D:
            EndMode3D();
            break;
        case RAY_CMD_BEGIN_TEXTURE:
            BeginTextureMode(*(RenderTexture*)command->payload);
            break;
        case RAY_CMD_END_TEXTURE:
            EndTextureMode();
            break;
        case RAY_CMD_MODEL: {
            RayModelCommand* payload = command->payload;
            if (payload->wires)
                DrawModelWiresEx(payload->model, payload->position, payload->rotationAxis,
                    payload->angle, payload->scale, command->color);
            else
                DrawModelEx(payload->model, payload->position, payload->rotationAxis,
                    payload->angle, payload->scale, command->color);
            break;
        }
        case RAY_CMD_BILLBOARD: {
            RayBillboardCommand* payload = command->payload;
            DrawBillboard(payload->camera, payload->texture, payload->position, payload->scale, command->color);
            break;
        }
        }
    }
    EndDrawing();
    flush_deferred_unloads();
    clear_commands(vm);
    /* poll_now processes ready async work without sleeping until a pending
       timer is due, which would otherwise stall the frame. A poll hook may
       close the window, so report the window state rather than always true. */
    lt->poll_now(vm);
    lt->push(vm, window_open ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_clear(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt->runtime_error(vm, "Expected a Color for ray.clear!");
    lt_Value color = lt->pop(vm);
    if (!window_open) lt->runtime_error(vm, "Expected ray.open before ray.clear!");
    Color clear = expect_color(vm, color, "Expected a Color for ray.clear!");
    require_draw_target(vm);
    RayCommand command = new_command(RAY_CMD_CLEAR);
    command.color = clear;
    push_command(command);
    return 0;
}

static uint8_t native_rect(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a Rectangle and a Color for ray.rect!");
    lt_Value color_value = lt->pop(vm);
    lt_Value bounds_value = lt->pop(vm);
    Color color = expect_color(vm, color_value, "Expected a Color for ray.rect!");
    LtRectangle* bounds = rectangle_data(vm, bounds_value, "Expected a Rectangle for ray.rect!");
    RayCommand command = new_command(RAY_CMD_RECT);
    command.x = (int)bounds->x;
    command.y = (int)bounds->y;
    command.w = (int)bounds->width;
    command.h = (int)bounds->height;
    command.color = color;
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
    RayCommand command = new_command(RAY_CMD_TEXT);
    command.x = (int)position->x;
    command.y = (int)position->y;
    command.size = size;
    command.color = color;
    const char* source = lt->get_string(vm, message);
    command.text = malloc(strlen(source) + 1);
    if (!command.text) lt->runtime_error(vm, "Out of memory!");
    memcpy(command.text, source, strlen(source) + 1);
    push_command(command);
    return 0;
}

#define RAY_BUTTON_QUERY(function_name, api_call, function_label, value_label)              \
    static uint8_t function_name(lt_VM* vm, uint8_t argc)                                   \
    {                                                                                       \
        if (argc != 1) lt->runtime_error(vm, "Expected a " value_label " for ray." function_label "!"); \
        lt_Value code = lt->pop(vm);                                                        \
        expect_number(vm, code, "Expected a " value_label " number!");                       \
        lt->push(vm, api_call((int)lt->get_number(code)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);  \
        return 1;                                                                            \
    }

RAY_BUTTON_QUERY(native_key_pressed, IsKeyPressed, "keyPressed", "key code")
RAY_BUTTON_QUERY(native_key_down, IsKeyDown, "keyDown", "key code")
RAY_BUTTON_QUERY(native_key_up, IsKeyUp, "keyUp", "key code")
RAY_BUTTON_QUERY(native_key_released, IsKeyReleased, "keyReleased", "key code")
RAY_BUTTON_QUERY(native_mouse_pressed, IsMouseButtonPressed, "mousePressed", "mouse button")
RAY_BUTTON_QUERY(native_mouse_down, IsMouseButtonDown, "mouseDown", "mouse button")
RAY_BUTTON_QUERY(native_mouse_released, IsMouseButtonReleased, "mouseReleased", "mouse button")

static uint8_t native_char_pressed(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.charPressed!");
    lt->push(vm, lt->make_number((double)GetCharPressed()));
    return 1;
}

static uint8_t native_mouse_delta(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.mouseDelta!");
    Vector2 delta = GetMouseDelta();
    lt->push(vm, make_vector2(vm, (double)delta.x, (double)delta.y));
    return 1;
}

static uint8_t native_mouse_wheel(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.mouseWheel!");
    lt->push(vm, lt->make_number((double)GetMouseWheelMove()));
    return 1;
}

static uint8_t native_gamepad_button_down(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a gamepad index and a button for ray.gamepadButtonDown!");
    lt_Value button = lt->pop(vm);
    lt_Value gamepad = lt->pop(vm);
    expect_number(vm, gamepad, "Expected a gamepad index number!");
    expect_number(vm, button, "Expected a gamepad button number!");
    lt->push(vm, IsGamepadButtonDown((int)lt->get_number(gamepad), (int)lt->get_number(button)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_gamepad_button_pressed(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a gamepad index and a button for ray.gamepadButtonPressed!");
    lt_Value button = lt->pop(vm);
    lt_Value gamepad = lt->pop(vm);
    expect_number(vm, gamepad, "Expected a gamepad index number!");
    expect_number(vm, button, "Expected a gamepad button number!");
    lt->push(vm, IsGamepadButtonPressed((int)lt->get_number(gamepad), (int)lt->get_number(button)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t native_gamepad_axis(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt->runtime_error(vm, "Expected a gamepad index and an axis for ray.gamepadAxis!");
    lt_Value axis = lt->pop(vm);
    lt_Value gamepad = lt->pop(vm);
    expect_number(vm, gamepad, "Expected a gamepad index number!");
    expect_number(vm, axis, "Expected a gamepad axis number!");
    lt->push(vm, lt->make_number((double)GetGamepadAxisMovement((int)lt->get_number(gamepad), (int)lt->get_number(axis))));
    return 1;
}

static uint8_t native_time(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.time!");
    lt->push(vm, lt->make_number(GetTime()));
    return 1;
}

static uint8_t native_fps(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments to ray.fps!");
    lt->push(vm, lt->make_number((double)GetFPS()));
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
    queued_resources = lt->make_array(vm);
    lt->table_set(vm, module_value, lt->make_string(vm, "__queued"), queued_resources);

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
    RAY_CLASS_METHOD(vector2_class, "rotate", vector2_rotate);
    RAY_CLASS_METHOD(vector2_class, "lerp", vector2_lerp);
    RAY_CLASS_METHOD(vector2_class, "reflect", vector2_reflect);
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
    RAY_CLASS_METHOD(vector3_class, "lerp", vector3_lerp);
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

    /* Resource types are created by their loaders, so they register no constructor. */
    image_class = lt->class_create(vm, "Image");
    lt->class_set_native_data_destroy(vm, image_class, destroy_image);
    RAY_CLASS_GETTER(image_class, "width", image_get_width);
    RAY_CLASS_GETTER(image_class, "height", image_get_height);
    RAY_CLASS_METHOD(image_class, "unload", image_unload);
    RAY_CLASS_METHOD(image_class, "export", image_export);
    RAY_CLASS_METHOD(image_class, "colorAt", image_color_at);
    RAY_CLASS_METHOD(image_class, "toString", image_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Image"), image_class);

    texture_class = lt->class_create(vm, "Texture");
    lt->class_set_native_data_destroy(vm, texture_class, destroy_texture);
    RAY_CLASS_GETTER(texture_class, "width", texture_get_width);
    RAY_CLASS_GETTER(texture_class, "height", texture_get_height);
    RAY_CLASS_METHOD(texture_class, "draw", texture_draw);
    RAY_CLASS_METHOD(texture_class, "drawRec", texture_draw_rec);
    RAY_CLASS_METHOD(texture_class, "drawPro", texture_draw_pro);
    RAY_CLASS_METHOD(texture_class, "unload", texture_unload);
    RAY_CLASS_METHOD(texture_class, "toString", texture_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Texture"), texture_class);

    font_class = lt->class_create(vm, "Font");
    lt->class_set_native_data_destroy(vm, font_class, destroy_font);
    RAY_CLASS_GETTER(font_class, "baseSize", font_get_base_size);
    RAY_CLASS_GETTER(font_class, "glyphCount", font_get_glyph_count);
    RAY_CLASS_METHOD(font_class, "measure", font_measure);
    RAY_CLASS_METHOD(font_class, "draw", font_draw);
    RAY_CLASS_METHOD(font_class, "unload", font_unload);
    RAY_CLASS_METHOD(font_class, "toString", font_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Font"), font_class);

    camera2d_class = lt->class_create(vm, "Camera2D");
    lt->class_set_native_data_destroy(vm, camera2d_class, destroy_native_data);
    lt->class_set_constructor(vm, camera2d_class, camera2d_constructor);
    RAY_CLASS_GETTER(camera2d_class, "target", camera2d_get_target);
    RAY_CLASS_SETTER(camera2d_class, "target", camera2d_set_target);
    RAY_CLASS_GETTER(camera2d_class, "offset", camera2d_get_offset);
    RAY_CLASS_SETTER(camera2d_class, "offset", camera2d_set_offset);
    RAY_CLASS_GETTER(camera2d_class, "rotation", camera2d_get_rotation);
    RAY_CLASS_SETTER(camera2d_class, "rotation", camera2d_set_rotation);
    RAY_CLASS_GETTER(camera2d_class, "zoom", camera2d_get_zoom);
    RAY_CLASS_SETTER(camera2d_class, "zoom", camera2d_set_zoom);
    RAY_CLASS_METHOD(camera2d_class, "toString", camera2d_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Camera2D"), camera2d_class);

    camera3d_class = lt->class_create(vm, "Camera3D");
    lt->class_set_native_data_destroy(vm, camera3d_class, destroy_native_data);
    lt->class_set_constructor(vm, camera3d_class, camera3d_constructor);
    RAY_CLASS_GETTER(camera3d_class, "position", camera3d_get_position);
    RAY_CLASS_SETTER(camera3d_class, "position", camera3d_set_position);
    RAY_CLASS_GETTER(camera3d_class, "target", camera3d_get_target);
    RAY_CLASS_SETTER(camera3d_class, "target", camera3d_set_target);
    RAY_CLASS_GETTER(camera3d_class, "up", camera3d_get_up);
    RAY_CLASS_SETTER(camera3d_class, "up", camera3d_set_up);
    RAY_CLASS_GETTER(camera3d_class, "fovy", camera3d_get_fovy);
    RAY_CLASS_SETTER(camera3d_class, "fovy", camera3d_set_fovy);
    RAY_CLASS_GETTER(camera3d_class, "projection", camera3d_get_projection);
    RAY_CLASS_SETTER(camera3d_class, "projection", camera3d_set_projection);
    RAY_CLASS_METHOD(camera3d_class, "toString", camera3d_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Camera3D"), camera3d_class);

    ray_class = lt->class_create(vm, "Ray");
    lt->class_set_native_data_destroy(vm, ray_class, destroy_native_data);
    lt->class_set_constructor(vm, ray_class, ray_constructor);
    RAY_CLASS_GETTER(ray_class, "position", ray_get_position);
    RAY_CLASS_SETTER(ray_class, "position", ray_set_position);
    RAY_CLASS_GETTER(ray_class, "direction", ray_get_direction);
    RAY_CLASS_SETTER(ray_class, "direction", ray_set_direction);
    RAY_CLASS_METHOD(ray_class, "toString", ray_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Ray"), ray_class);

    /* Ray collisions are produced by the raycasts, so they have no constructor. */
    ray_collision_class = lt->class_create(vm, "RayCollision");
    lt->class_set_native_data_destroy(vm, ray_collision_class, destroy_native_data);
    RAY_CLASS_GETTER(ray_collision_class, "hit", ray_collision_get_hit);
    RAY_CLASS_GETTER(ray_collision_class, "distance", ray_collision_get_distance);
    RAY_CLASS_GETTER(ray_collision_class, "point", ray_collision_get_point);
    RAY_CLASS_GETTER(ray_collision_class, "normal", ray_collision_get_normal);
    RAY_CLASS_METHOD(ray_collision_class, "toString", ray_collision_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "RayCollision"), ray_collision_class);

    /* Meshes, models, and render textures are created by their loaders. */
    mesh_class = lt->class_create(vm, "Mesh");
    lt->class_set_native_data_destroy(vm, mesh_class, destroy_mesh);
    RAY_CLASS_GETTER(mesh_class, "vertexCount", mesh_get_vertex_count);
    RAY_CLASS_GETTER(mesh_class, "triangleCount", mesh_get_triangle_count);
    RAY_CLASS_METHOD(mesh_class, "unload", mesh_unload);
    RAY_CLASS_METHOD(mesh_class, "toString", mesh_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Mesh"), mesh_class);

    model_class = lt->class_create(vm, "Model");
    lt->class_set_native_data_destroy(vm, model_class, destroy_model);
    RAY_CLASS_GETTER(model_class, "meshCount", model_get_mesh_count);
    RAY_CLASS_GETTER(model_class, "materialCount", model_get_material_count);
    RAY_CLASS_METHOD(model_class, "draw", model_draw);
    RAY_CLASS_METHOD(model_class, "drawEx", model_draw_ex);
    RAY_CLASS_METHOD(model_class, "drawWires", model_draw_wires);
    RAY_CLASS_METHOD(model_class, "drawWiresEx", model_draw_wires_ex);
    RAY_CLASS_METHOD(model_class, "unload", model_unload);
    RAY_CLASS_METHOD(model_class, "toString", model_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "Model"), model_class);

    render_texture_class = lt->class_create(vm, "RenderTexture");
    lt->class_set_native_data_destroy(vm, render_texture_class, destroy_render_texture);
    RAY_CLASS_GETTER(render_texture_class, "width", render_texture_get_width);
    RAY_CLASS_GETTER(render_texture_class, "height", render_texture_get_height);
    RAY_CLASS_METHOD(render_texture_class, "draw", render_texture_draw);
    RAY_CLASS_METHOD(render_texture_class, "drawPro", render_texture_draw_pro);
    RAY_CLASS_METHOD(render_texture_class, "image", render_texture_image);
    RAY_CLASS_METHOD(render_texture_class, "unload", render_texture_unload);
    RAY_CLASS_METHOD(render_texture_class, "toString", render_texture_to_string);
    lt->table_set(vm, module_value, lt->make_string(vm, "RenderTexture"), render_texture_class);

    set_native(vm, module_value, "loadImage", native_load_image);
    set_native(vm, module_value, "genImageColor", native_gen_image_color);
    set_native(vm, module_value, "loadTexture", native_load_texture);
    set_native(vm, module_value, "loadTextureFromImage", native_load_texture_from_image);
    set_native(vm, module_value, "loadFont", native_load_font);
    set_native(vm, module_value, "defaultFont", native_default_font);
    set_native(vm, module_value, "genMeshCube", native_gen_mesh_cube);
    set_native(vm, module_value, "genMeshSphere", native_gen_mesh_sphere);
    set_native(vm, module_value, "genMeshPlane", native_gen_mesh_plane);
    set_native(vm, module_value, "genMeshCylinder", native_gen_mesh_cylinder);
    set_native(vm, module_value, "genMeshTorus", native_gen_mesh_torus);
    set_native(vm, module_value, "genMeshKnot", native_gen_mesh_knot);
    set_native(vm, module_value, "loadModel", native_load_model);
    set_native(vm, module_value, "modelFromMesh", native_model_from_mesh);
    set_native(vm, module_value, "loadRenderTexture", native_load_render_texture);
    set_native(vm, module_value, "windowSize", native_window_size);
    set_native(vm, module_value, "traceLog", native_trace_log);

    set_native(vm, module_value, "open", native_open);
    set_native(vm, module_value, "close", native_close);
    set_native(vm, module_value, "on", native_on);
    set_native(vm, module_value, "update", native_update);
    set_native(vm, module_value, "clear", native_clear);
    set_native(vm, module_value, "rect", native_rect);
    set_native(vm, module_value, "text", native_text);
    set_native(vm, module_value, "rectLines", native_rect_lines);
    set_native(vm, module_value, "circle", native_circle);
    set_native(vm, module_value, "circleLines", native_circle_lines);
    set_native(vm, module_value, "line", native_line);
    set_native(vm, module_value, "lineEx", native_line_ex);
    set_native(vm, module_value, "triangle", native_triangle);
    set_native(vm, module_value, "triangleLines", native_triangle_lines);
    set_native(vm, module_value, "polygon", native_polygon);
    set_native(vm, module_value, "polygonLines", native_polygon_lines);
    set_native(vm, module_value, "ring", native_ring);
    set_native(vm, module_value, "cube", native_cube);
    set_native(vm, module_value, "cubeWires", native_cube_wires);
    set_native(vm, module_value, "sphere", native_sphere);
    set_native(vm, module_value, "sphereWires", native_sphere_wires);
    set_native(vm, module_value, "cylinder", native_cylinder);
    set_native(vm, module_value, "cylinderWires", native_cylinder_wires);
    set_native(vm, module_value, "capsule", native_capsule);
    set_native(vm, module_value, "grid", native_grid);
    set_native(vm, module_value, "line3D", native_line3d);
    set_native(vm, module_value, "point3D", native_point3d);
    set_native(vm, module_value, "triangle3D", native_triangle3d);
    set_native(vm, module_value, "plane", native_plane);
    set_native(vm, module_value, "boundingBox", native_bounding_box);
    set_native(vm, module_value, "billboard", native_billboard);
    set_native(vm, module_value, "beginMode3D", native_begin_mode3d);
    set_native(vm, module_value, "endMode3D", native_end_mode3d);
    set_native(vm, module_value, "beginTextureMode", native_begin_texture_mode);
    set_native(vm, module_value, "endTextureMode", native_end_texture_mode);
    set_native(vm, module_value, "beginMode2D", native_begin_mode2d);
    set_native(vm, module_value, "endMode2D", native_end_mode2d);
    set_native(vm, module_value, "keyPressed", native_key_pressed);
    set_native(vm, module_value, "keyDown", native_key_down);
    set_native(vm, module_value, "keyUp", native_key_up);
    set_native(vm, module_value, "keyReleased", native_key_released);
    set_native(vm, module_value, "mousePressed", native_mouse_pressed);
    set_native(vm, module_value, "mouseDown", native_mouse_down);
    set_native(vm, module_value, "mouseReleased", native_mouse_released);
    set_native(vm, module_value, "mouse", native_mouse_position);
    set_native(vm, module_value, "mouseDelta", native_mouse_delta);
    set_native(vm, module_value, "mouseWheel", native_mouse_wheel);
    set_native(vm, module_value, "charPressed", native_char_pressed);
    set_native(vm, module_value, "gamepadButtonDown", native_gamepad_button_down);
    set_native(vm, module_value, "gamepadButtonPressed", native_gamepad_button_pressed);
    set_native(vm, module_value, "gamepadAxis", native_gamepad_axis);
    set_native(vm, module_value, "setFPS", native_set_fps);
    set_native(vm, module_value, "time", native_time);
    set_native(vm, module_value, "fps", native_fps);
    set_native(vm, module_value, "checkCollisionRecs", native_check_collision_recs);
    set_native(vm, module_value, "checkCollisionCircles", native_check_collision_circles);
    set_native(vm, module_value, "checkCollisionPointRec", native_check_collision_point_rec);
    set_native(vm, module_value, "checkCollisionPointCircle", native_check_collision_point_circle);
    set_native(vm, module_value, "getCollisionRec", native_get_collision_rec);
    set_native(vm, module_value, "fade", native_fade);
    set_native(vm, module_value, "colorLerp", native_color_lerp);
    set_native(vm, module_value, "colorBrightness", native_color_brightness);
    set_native(vm, module_value, "colorTint", native_color_tint);
    set_native(vm, module_value, "colorFromHSV", native_color_from_hsv);
    set_native(vm, module_value, "colorToInt", native_color_to_int);
    set_native(vm, module_value, "worldToScreen", native_world_to_screen);
    set_native(vm, module_value, "screenToWorld", native_screen_to_world);
    set_native(vm, module_value, "getWorldToScreen", native_get_world_to_screen);
    set_native(vm, module_value, "getScreenToWorldRay", native_get_screen_to_world_ray);
    set_native(vm, module_value, "raycastSphere", native_raycast_sphere);
    set_native(vm, module_value, "raycastBox", native_raycast_box);
    set_native(vm, module_value, "setWindowTitle", native_set_window_title);
    set_native(vm, module_value, "setWindowSize", native_set_window_size);
    set_native(vm, module_value, "toggleFullscreen", native_toggle_fullscreen);
    set_native(vm, module_value, "screenshot", native_screenshot);

    lt->table_set(vm, module_value, lt->make_string(vm, "version"), lt->make_string(vm, RAYLIB_VERSION));

    lt_Value keys = lt->make_table(vm);
    set_number(vm, keys, "none", 0);
    set_number(vm, keys, "apostrophe", 39);
    set_number(vm, keys, "comma", 44);
    set_number(vm, keys, "minus", 45);
    set_number(vm, keys, "period", 46);
    set_number(vm, keys, "slash", 47);
    set_number(vm, keys, "zero", 48);
    set_number(vm, keys, "one", 49);
    set_number(vm, keys, "two", 50);
    set_number(vm, keys, "three", 51);
    set_number(vm, keys, "four", 52);
    set_number(vm, keys, "five", 53);
    set_number(vm, keys, "six", 54);
    set_number(vm, keys, "seven", 55);
    set_number(vm, keys, "eight", 56);
    set_number(vm, keys, "nine", 57);
    set_number(vm, keys, "semicolon", 59);
    set_number(vm, keys, "equal", 61);
    set_number(vm, keys, "a", 65);
    set_number(vm, keys, "b", 66);
    set_number(vm, keys, "c", 67);
    set_number(vm, keys, "d", 68);
    set_number(vm, keys, "e", 69);
    set_number(vm, keys, "f", 70);
    set_number(vm, keys, "g", 71);
    set_number(vm, keys, "h", 72);
    set_number(vm, keys, "i", 73);
    set_number(vm, keys, "j", 74);
    set_number(vm, keys, "k", 75);
    set_number(vm, keys, "l", 76);
    set_number(vm, keys, "m", 77);
    set_number(vm, keys, "n", 78);
    set_number(vm, keys, "o", 79);
    set_number(vm, keys, "p", 80);
    set_number(vm, keys, "q", 81);
    set_number(vm, keys, "r", 82);
    set_number(vm, keys, "s", 83);
    set_number(vm, keys, "t", 84);
    set_number(vm, keys, "u", 85);
    set_number(vm, keys, "v", 86);
    set_number(vm, keys, "w", 87);
    set_number(vm, keys, "x", 88);
    set_number(vm, keys, "y", 89);
    set_number(vm, keys, "z", 90);
    set_number(vm, keys, "leftBracket", 91);
    set_number(vm, keys, "backslash", 92);
    set_number(vm, keys, "rightBracket", 93);
    set_number(vm, keys, "grave", 96);
    set_number(vm, keys, "space", 32);
    set_number(vm, keys, "escape", 256);
    set_number(vm, keys, "enter", 257);
    set_number(vm, keys, "tab", 258);
    set_number(vm, keys, "backspace", 259);
    set_number(vm, keys, "insert", 260);
    set_number(vm, keys, "delete", 261);
    set_number(vm, keys, "right", 262);
    set_number(vm, keys, "left", 263);
    set_number(vm, keys, "down", 264);
    set_number(vm, keys, "up", 265);
    set_number(vm, keys, "pageUp", 266);
    set_number(vm, keys, "pageDown", 267);
    set_number(vm, keys, "home", 268);
    set_number(vm, keys, "end", 269);
    set_number(vm, keys, "capsLock", 280);
    set_number(vm, keys, "scrollLock", 281);
    set_number(vm, keys, "numLock", 282);
    set_number(vm, keys, "printScreen", 283);
    set_number(vm, keys, "pause", 284);
    set_number(vm, keys, "f1", 290);
    set_number(vm, keys, "f2", 291);
    set_number(vm, keys, "f3", 292);
    set_number(vm, keys, "f4", 293);
    set_number(vm, keys, "f5", 294);
    set_number(vm, keys, "f6", 295);
    set_number(vm, keys, "f7", 296);
    set_number(vm, keys, "f8", 297);
    set_number(vm, keys, "f9", 298);
    set_number(vm, keys, "f10", 299);
    set_number(vm, keys, "f11", 300);
    set_number(vm, keys, "f12", 301);
    set_number(vm, keys, "leftShift", 340);
    set_number(vm, keys, "leftControl", 341);
    set_number(vm, keys, "leftAlt", 342);
    set_number(vm, keys, "leftSuper", 343);
    set_number(vm, keys, "rightShift", 344);
    set_number(vm, keys, "rightControl", 345);
    set_number(vm, keys, "rightAlt", 346);
    set_number(vm, keys, "rightSuper", 347);
    set_number(vm, keys, "kbMenu", 348);
    set_number(vm, keys, "kp0", 320);
    set_number(vm, keys, "kp1", 321);
    set_number(vm, keys, "kp2", 322);
    set_number(vm, keys, "kp3", 323);
    set_number(vm, keys, "kp4", 324);
    set_number(vm, keys, "kp5", 325);
    set_number(vm, keys, "kp6", 326);
    set_number(vm, keys, "kp7", 327);
    set_number(vm, keys, "kp8", 328);
    set_number(vm, keys, "kp9", 329);
    set_number(vm, keys, "kpDecimal", 330);
    set_number(vm, keys, "kpDivide", 331);
    set_number(vm, keys, "kpMultiply", 332);
    set_number(vm, keys, "kpSubtract", 333);
    set_number(vm, keys, "kpAdd", 334);
    set_number(vm, keys, "kpEnter", 335);
    set_number(vm, keys, "kpEqual", 336);
    set_number(vm, keys, "back", 4);
    set_number(vm, keys, "menu", 5);
    set_number(vm, keys, "volumeUp", 24);
    set_number(vm, keys, "volumeDown", 25);
    lt->table_set(vm, module_value, lt->make_string(vm, "keys"), keys);

    lt_Value mouse_buttons = lt->make_table(vm);
    set_number(vm, mouse_buttons, "left", 0);
    set_number(vm, mouse_buttons, "right", 1);
    set_number(vm, mouse_buttons, "middle", 2);
    set_number(vm, mouse_buttons, "side", 3);
    set_number(vm, mouse_buttons, "extra", 4);
    set_number(vm, mouse_buttons, "forward", 5);
    set_number(vm, mouse_buttons, "back", 6);
    lt->table_set(vm, module_value, lt->make_string(vm, "mouseButtons"), mouse_buttons);

    lt_Value gamepad_buttons = lt->make_table(vm);
    set_number(vm, gamepad_buttons, "unknown", 0);
    set_number(vm, gamepad_buttons, "leftFaceUp", 1);
    set_number(vm, gamepad_buttons, "leftFaceRight", 2);
    set_number(vm, gamepad_buttons, "leftFaceDown", 3);
    set_number(vm, gamepad_buttons, "leftFaceLeft", 4);
    set_number(vm, gamepad_buttons, "rightFaceUp", 5);
    set_number(vm, gamepad_buttons, "rightFaceRight", 6);
    set_number(vm, gamepad_buttons, "rightFaceDown", 7);
    set_number(vm, gamepad_buttons, "rightFaceLeft", 8);
    set_number(vm, gamepad_buttons, "leftTrigger1", 9);
    set_number(vm, gamepad_buttons, "leftTrigger2", 10);
    set_number(vm, gamepad_buttons, "rightTrigger1", 11);
    set_number(vm, gamepad_buttons, "rightTrigger2", 12);
    set_number(vm, gamepad_buttons, "middleLeft", 13);
    set_number(vm, gamepad_buttons, "middle", 14);
    set_number(vm, gamepad_buttons, "middleRight", 15);
    set_number(vm, gamepad_buttons, "leftThumb", 16);
    set_number(vm, gamepad_buttons, "rightThumb", 17);

    lt_Value gamepad_axes = lt->make_table(vm);
    set_number(vm, gamepad_axes, "leftX", 0);
    set_number(vm, gamepad_axes, "leftY", 1);
    set_number(vm, gamepad_axes, "rightX", 2);
    set_number(vm, gamepad_axes, "rightY", 3);
    set_number(vm, gamepad_axes, "leftTrigger", 4);
    set_number(vm, gamepad_axes, "rightTrigger", 5);

    lt_Value gamepad = lt->make_table(vm);
    lt->table_set(vm, gamepad, lt->make_string(vm, "buttons"), gamepad_buttons);
    lt->table_set(vm, gamepad, lt->make_string(vm, "axes"), gamepad_axes);
    lt->table_set(vm, module_value, lt->make_string(vm, "gamepad"), gamepad);

    lt_Value log_levels = lt->make_table(vm);
    set_number(vm, log_levels, "all", 0);
    set_number(vm, log_levels, "trace", 1);
    set_number(vm, log_levels, "debug", 2);
    set_number(vm, log_levels, "info", 3);
    set_number(vm, log_levels, "warning", 4);
    set_number(vm, log_levels, "error", 5);
    set_number(vm, log_levels, "fatal", 6);
    set_number(vm, log_levels, "none", 7);
    lt->table_set(vm, module_value, lt->make_string(vm, "log"), log_levels);

    lt_Value projection = lt->make_table(vm);
    set_number(vm, projection, "perspective", CAMERA_PERSPECTIVE);
    set_number(vm, projection, "orthographic", CAMERA_ORTHOGRAPHIC);
    lt->table_set(vm, module_value, lt->make_string(vm, "projection"), projection);

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
