#include "../../src/little.h"
#include "../../vendor/pdcursesmod/curses.h"

#include <stdint.h>
#include <string.h>

/* This keeps the loadLibrary ABI even when linked into little.exe. */

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#define TERM_MAX_LINE 4096
#define TERM_HISTORY_CAPACITY 64

typedef struct
{
    lt_VM* vm;
    lt_Value module;
    lt_Value callback;
    uint32_t poll_hook;
    uint8_t active;
    uint8_t callback_set;
    char history[TERM_HISTORY_CAPACITY][TERM_MAX_LINE];
    uint8_t history_count;
} TermState;

static const lt_Api* lt = 0;
static TermState state;

static void require_args(lt_VM* vm, uint8_t argc, uint8_t expected, const char* message)
{
    if (argc != expected) lt->runtime_error(vm, message);
}

static void require_active(lt_VM* vm)
{
    if (!state.active || state.vm != vm) lt->runtime_error(vm, "Call term.open before using the terminal!");
}

static uint8_t is_callable(lt_Value value)
{
    return LT_IS_FUNCTION(value) || LT_IS_CLOSURE(value) || LT_IS_NATIVE(value) || LT_IS_CLASS(value);
}

static const char* event_key_name(int key)
{
    switch (key)
    {
    case '\r': case '\n': case KEY_ENTER: return "enter";
    case 27: return "escape";
    case '\t': return "tab";
    case KEY_BACKSPACE: case 8: case 127: return "backspace";
    case KEY_LEFT: return "left";
    case KEY_RIGHT: return "right";
    case KEY_UP: return "up";
    case KEY_DOWN: return "down";
    case KEY_HOME: return "home";
    case KEY_END: return "end";
    case KEY_DC: return "delete";
    case KEY_NPAGE: return "page-down";
    case KEY_PPAGE: return "page-up";
    case KEY_RESIZE: return "resize";
    case 3: return "ctrl-c";
    case 4: return "ctrl-d";
    default: return 0;
    }
}

static lt_Value make_event(lt_VM* vm, int key)
{
    lt_Value event = lt->make_table(vm);
    const char* name = event_key_name(key);
    lt->table_set(vm, event, lt->make_string(vm, "code"), lt->make_number((double)key));
    if (key == KEY_RESIZE)
    {
        lt->table_set(vm, event, lt->make_string(vm, "type"), lt->make_string(vm, "resize"));
        return event;
    }
    if (key >= 32 && key <= 255)
    {
        char text[2] = { (char)key, 0 };
        lt->table_set(vm, event, lt->make_string(vm, "type"), lt->make_string(vm, "text"));
        lt->table_set(vm, event, lt->make_string(vm, "text"), lt->make_string(vm, text));
        return event;
    }
    lt->table_set(vm, event, lt->make_string(vm, "type"), lt->make_string(vm, "key"));
    lt->table_set(vm, event, lt->make_string(vm, "key"), lt->make_string(vm, name ? name : "unknown"));
    return event;
}

static int next_key(void)
{
    timeout(0);
    return getch();
}

/* One event per turn keeps terminal callbacks fair with timers and promises. */
static lt_PollResult term_poll_hook(lt_VM* vm, void* context)
{
    TermState* terminal = context;
    int key;
    if (!terminal->active || terminal->vm != vm) return LT_POLL_IDLE;
    key = next_key();
    if (key == ERR) return terminal->callback_set ? LT_POLL_PENDING : LT_POLL_IDLE;
    if (terminal->callback_set)
    {
        lt->push(vm, make_event(vm, key));
        uint16_t returns = lt->exec(vm, terminal->callback, 1);
        while (returns--) lt->pop(vm);
    }
    return LT_POLL_WORK;
}

uint8_t ltterm_update(lt_VM* vm)
{
    return term_poll_hook(vm, &state) == LT_POLL_WORK;
}

static uint8_t term_open(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.open!");
    if (!state.active)
    {
        if (!initscr()) lt->runtime_error(vm, "Unable to open terminal session!");
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        if (has_colors())
        {
            start_color();
            use_default_colors();
        }
        state.vm = vm;
        state.active = 1;
        state.poll_hook = lt->add_poll_hook(vm, term_poll_hook, &state);
    }
    else if (state.vm != vm) lt->runtime_error(vm, "The terminal is already owned by another Little VM!");
    lt->push(vm, LT_VALUE_TRUE);
    return 1;
}

static uint8_t term_close(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.close!");
    if (state.active && state.vm == vm)
    {
        lt->remove_poll_hook(vm, state.poll_hook);
        endwin();
    }
    if (state.module && state.vm == vm)
        lt->table_set(vm, state.module, lt->make_string(vm, "_callback"), LT_VALUE_NULL);
    state.active = 0;
    state.callback_set = 0;
    state.callback = LT_VALUE_NULL;
    state.vm = 0;
    state.poll_hook = 0;
    return 0;
}

static uint8_t term_is_open(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.isOpen!");
    lt->push(vm, state.active && state.vm == vm ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t term_size(lt_VM* vm, uint8_t argc)
{
    int rows, columns;
    require_args(vm, argc, 0, "Expected no arguments to term.size!");
    require_active(vm);
    getmaxyx(stdscr, rows, columns);
    lt_Value result = lt->make_table(vm);
    lt->table_set(vm, result, lt->make_string(vm, "width"), lt->make_number((double)columns));
    lt->table_set(vm, result, lt->make_string(vm, "height"), lt->make_number((double)rows));
    lt->push(vm, result);
    return 1;
}

static uint8_t term_clear(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.clear!");
    require_active(vm);
    erase();
    return 0;
}

static uint8_t term_clear_line(lt_VM* vm, uint8_t argc)
{
    lt_Value y;
    require_args(vm, argc, 1, "Expected y argument to term.clearLine!");
    y = lt->pop(vm);
    require_active(vm);
    if (!LT_IS_NUMBER(y)) lt->runtime_error(vm, "Expected number argument to term.clearLine!");
    move((int)lt->get_number(y), 0);
    clrtoeol();
    return 0;
}

static uint8_t term_present(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.present!");
    require_active(vm);
    refresh();
    return 0;
}

static uint8_t term_move(lt_VM* vm, uint8_t argc)
{
    lt_Value y, x;
    require_args(vm, argc, 2, "Expected x and y arguments to term.move!");
    y = lt->pop(vm);
    x = lt->pop(vm);
    require_active(vm);
    if (!LT_IS_NUMBER(x) || !LT_IS_NUMBER(y)) lt->runtime_error(vm, "Expected number arguments to term.move!");
    move((int)lt->get_number(y), (int)lt->get_number(x));
    return 0;
}

static uint8_t term_cursor(lt_VM* vm, uint8_t argc)
{
    lt_Value visible;
    require_args(vm, argc, 1, "Expected boolean argument to term.cursor!");
    visible = lt->pop(vm);
    require_active(vm);
    if (!LT_IS_BOOL(visible)) lt->runtime_error(vm, "Expected boolean argument to term.cursor!");
    curs_set(LT_IS_TRUE(visible) ? 1 : 0);
    return 0;
}

static uint8_t term_write(lt_VM* vm, uint8_t argc)
{
    lt_Value text, y, x;
    require_args(vm, argc, 3, "Expected x, y, and text arguments to term.write!");
    text = lt->pop(vm);
    y = lt->pop(vm);
    x = lt->pop(vm);
    require_active(vm);
    if (!LT_IS_NUMBER(x) || !LT_IS_NUMBER(y) || !LT_IS_STRING(text))
        lt->runtime_error(vm, "Expected number, number, and string arguments to term.write!");
    mvaddstr((int)lt->get_number(y), (int)lt->get_number(x), lt->get_string(vm, text));
    return 0;
}

static uint8_t term_bell(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.bell!");
    require_active(vm);
    beep();
    return 0;
}

static uint8_t term_poll(lt_VM* vm, uint8_t argc)
{
    int key;
    require_args(vm, argc, 0, "Expected no arguments to term.poll!");
    require_active(vm);
    key = next_key();
    lt->push(vm, key == ERR ? LT_VALUE_NULL : make_event(vm, key));
    return 1;
}

static uint8_t term_update_native(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.update!");
    require_active(vm);
    lt->push(vm, ltterm_update(vm) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t term_on_event(lt_VM* vm, uint8_t argc)
{
    lt_Value callback;
    require_args(vm, argc, 1, "Expected callback argument to term.onEvent!");
    callback = lt->pop(vm);
    require_active(vm);
    if (!is_callable(callback)) lt->runtime_error(vm, "Expected callable argument to term.onEvent!");
    state.callback = callback;
    state.callback_set = 1;
    /* Root the callback through the public module table.  The generic poller
       intentionally stores C-only data and does not participate in GC. */
    lt->table_set(vm, state.module, lt->make_string(vm, "_callback"), callback);
    lt->push(vm, callback);
    return 1;
}

static void redraw_line(const char* prompt, const char* line, uint32_t cursor)
{
    int y, x;
    getyx(stdscr, y, x);
    (void)x;
    move(y, 0);
    clrtoeol();
    addstr(prompt);
    addstr(line);
    move(y, (int)(strlen(prompt) + cursor));
    refresh();
}

static void history_push(const char* line)
{
    if (!line[0]) return;
    if (state.history_count > 0 && strcmp(state.history[state.history_count - 1], line) == 0) return;
    if (state.history_count == TERM_HISTORY_CAPACITY)
    {
        memmove(state.history, state.history + 1, (TERM_HISTORY_CAPACITY - 1) * TERM_MAX_LINE);
        state.history_count--;
    }
    strncpy(state.history[state.history_count], line, TERM_MAX_LINE - 1);
    state.history[state.history_count][TERM_MAX_LINE - 1] = 0;
    state.history_count++;
}

static uint8_t term_read_line(lt_VM* vm, uint8_t argc)
{
    lt_Value prompt_value;
    char line[TERM_MAX_LINE] = { 0 };
    uint32_t length = 0, cursor = 0;
    int history_index = -1;
    const char* prompt;

    require_args(vm, argc, 1, "Expected prompt argument to term.readLine!");
    prompt_value = lt->pop(vm);
    require_active(vm);
    if (!LT_IS_STRING(prompt_value)) lt->runtime_error(vm, "Expected string argument to term.readLine!");
    prompt = lt->get_string(vm, prompt_value);
    addstr(prompt);
    refresh();
    timeout(-1);
    for (;;)
    {
        int key = getch();
        if (key == ERR || key == 4 || key == 3)
        {
            addch('\n'); refresh(); timeout(0);
            lt->push(vm, LT_VALUE_NULL);
            return 1;
        }
        if (key == '\r' || key == '\n' || key == KEY_ENTER)
        {
            addch('\n'); refresh(); timeout(0);
            line[length] = 0;
            history_push(line);
            lt->push(vm, lt->make_string(vm, line));
            return 1;
        }
        if (key == KEY_LEFT && cursor > 0) cursor--;
        else if (key == KEY_RIGHT && cursor < length) cursor++;
        else if (key == KEY_HOME) cursor = 0;
        else if (key == KEY_END) cursor = length;
        else if ((key == KEY_BACKSPACE || key == 8 || key == 127) && cursor > 0)
        {
            memmove(line + cursor - 1, line + cursor, length - cursor + 1); cursor--; length--;
        }
        else if (key == KEY_DC && cursor < length)
        {
            memmove(line + cursor, line + cursor + 1, length - cursor); length--;
        }
        else if (key == KEY_UP && state.history_count > 0)
        {
            if (history_index < 0) history_index = state.history_count - 1;
            else if (history_index > 0) history_index--;
            strncpy(line, state.history[history_index], TERM_MAX_LINE);
            line[TERM_MAX_LINE - 1] = 0; length = cursor = (uint32_t)strlen(line);
        }
        else if (key == KEY_DOWN && history_index >= 0)
        {
            history_index++;
            if (history_index >= state.history_count) { history_index = -1; line[0] = 0; }
            else { strncpy(line, state.history[history_index], TERM_MAX_LINE); line[TERM_MAX_LINE - 1] = 0; }
            length = cursor = (uint32_t)strlen(line);
        }
        else if (key >= 32 && key <= 255 && length + 1 < TERM_MAX_LINE)
        {
            memmove(line + cursor + 1, line + cursor, length - cursor + 1);
            line[cursor++] = (char)key; length++; history_index = -1;
        }
        redraw_line(prompt, line, cursor);
    }
}

void lt_term_shutdown(void)
{
    if (state.active) endwin();
    memset(&state, 0, sizeof(state));
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    lt_Value term;
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api)) return LT_VALUE_NULL;
    lt = api;
    term = lt->make_table(vm);
    state.module = term;
#define TERM_FN(name, function) lt->table_set(vm, term, lt->make_string(vm, name), lt->make_native(vm, function))
    TERM_FN("open", term_open); TERM_FN("close", term_close); TERM_FN("isOpen", term_is_open);
    TERM_FN("size", term_size); TERM_FN("clear", term_clear); TERM_FN("clearLine", term_clear_line);
    TERM_FN("present", term_present); TERM_FN("move", term_move); TERM_FN("cursor", term_cursor);
    TERM_FN("write", term_write); TERM_FN("bell", term_bell); TERM_FN("poll", term_poll);
    TERM_FN("update", term_update_native); TERM_FN("onEvent", term_on_event); TERM_FN("readLine", term_read_line);
#undef TERM_FN
    return term;
}
