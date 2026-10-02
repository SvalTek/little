#include "little.h"

#include "raylib.h"

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

static uint8_t color_channel(lt_VM* vm, lt_Value channel, const char* message)
{
    expect_number(vm, channel, message);
    double n = lt->get_number(channel);
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    return (uint8_t)n;
}

static Color expect_color(lt_VM* vm, lt_Value value, const char* message)
{
    Color color = { 255, 255, 255, 255 };
    if (!LT_IS_TABLE(value)) lt->runtime_error(vm, message);
    color.r = color_channel(vm, lt->table_get(vm, value, lt->make_string(vm, "r")), message);
    color.g = color_channel(vm, lt->table_get(vm, value, lt->make_string(vm, "g")), message);
    color.b = color_channel(vm, lt->table_get(vm, value, lt->make_string(vm, "b")), message);
    lt_Value alpha = lt->table_get(vm, value, lt->make_string(vm, "a"));
    if (!LT_IS_NULL(alpha)) color.a = color_channel(vm, alpha, message);
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
    if (argc != 1) lt->runtime_error(vm, "Expected color for ray.clear!");
    lt_Value color = lt->pop(vm);
    if (!window_open) lt->runtime_error(vm, "Expected ray.open before ray.clear!");
    clear_color = expect_color(vm, color, "Expected ray color table with r, g, b!");
    has_clear = 1;
    return 0;
}

static uint8_t native_rect(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "Expected x, y, width, height, and color for ray.rect!");
    lt_Value color = lt->pop(vm);
    lt_Value height = lt->pop(vm);
    lt_Value width = lt->pop(vm);
    lt_Value y = lt->pop(vm);
    lt_Value x = lt->pop(vm);
    expect_number(vm, x, "Expected ray rect x to be number!");
    expect_number(vm, y, "Expected ray rect y to be number!");
    expect_number(vm, width, "Expected ray rect width to be number!");
    expect_number(vm, height, "Expected ray rect height to be number!");
    RayCommand command;
    command.kind = RAY_CMD_RECT;
    command.x = (int)lt->get_number(x);
    command.y = (int)lt->get_number(y);
    command.w = (int)lt->get_number(width);
    command.h = (int)lt->get_number(height);
    command.size = 0;
    command.color = expect_color(vm, color, "Expected ray color table with r, g, b!");
    command.text = 0;
    require_draw_target(vm);
    push_command(command);
    return 0;
}

static uint8_t native_text(lt_VM* vm, uint8_t argc)
{
    if (argc != 5) lt->runtime_error(vm, "Expected text, x, y, size, and color for ray.text!");
    lt_Value color = lt->pop(vm);
    lt_Value size = lt->pop(vm);
    lt_Value y = lt->pop(vm);
    lt_Value x = lt->pop(vm);
    lt_Value message = lt->pop(vm);
    expect_string(vm, message, "Expected ray text to be string!");
    expect_number(vm, x, "Expected ray text x to be number!");
    expect_number(vm, y, "Expected ray text y to be number!");
    expect_number(vm, size, "Expected ray text size to be number!");
    RayCommand command;
    command.kind = RAY_CMD_TEXT;
    command.x = (int)lt->get_number(x);
    command.y = (int)lt->get_number(y);
    command.w = 0;
    command.h = 0;
    command.size = (int)lt->get_number(size);
    if (command.size <= 0) lt->runtime_error(vm, "Expected ray text size to be positive!");
    command.color = expect_color(vm, color, "Expected ray color table with r, g, b!");
    require_draw_target(vm);
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

static uint8_t native_mouse(lt_VM* vm, uint8_t argc, uint8_t pressed)
{
    if (pressed)
    {
        if (argc != 1) lt->runtime_error(vm, "Expected mouse button for ray.mousePressed!");
        lt_Value button = lt->pop(vm);
        expect_number(vm, button, "Expected ray mouse button to be number!");
        lt->push(vm, IsMouseButtonPressed((int)lt->get_number(button)) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
        return 1;
    }
    if (argc != 0) lt->runtime_error(vm, "Expected no arguments!");
    lt_Value position = lt->make_table(vm);
    lt->table_set(vm, position, lt->make_string(vm, "x"), lt->make_number((double)GetMouseX()));
    lt->table_set(vm, position, lt->make_string(vm, "y"), lt->make_number((double)GetMouseY()));
    lt->push(vm, position);
    return 1;
}

static uint8_t native_mouse_pressed(lt_VM* vm, uint8_t argc)
{
    return native_mouse(vm, argc, 1);
}

static uint8_t native_mouse_position(lt_VM* vm, uint8_t argc)
{
    return native_mouse(vm, argc, 0);
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

    lt_Value white = lt->make_table(vm);
    set_number(vm, white, "r", 255);
    set_number(vm, white, "g", 255);
    set_number(vm, white, "b", 255);
    lt->table_set(vm, module_value, lt->make_string(vm, "white"), white);

    lt_Value black = lt->make_table(vm);
    set_number(vm, black, "r", 0);
    set_number(vm, black, "g", 0);
    set_number(vm, black, "b", 0);
    lt->table_set(vm, module_value, lt->make_string(vm, "black"), black);

    return module_value;
}
