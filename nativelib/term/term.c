#include "../../src/little.h"
#include "../../vendor/pdcursesmod/curses.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* This keeps the loadLibrary ABI even when linked into little.exe. */

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#define TERM_MAX_LINE 4096
#define TERM_HISTORY_CAPACITY 64
#define TERM_MAX_MODULES 8

typedef struct
{
    lt_VM* vm;
    lt_Value module;
    lt_Value callback;
    uint32_t poll_hook;
    uint8_t active;
    uint8_t callback_set;
    int output_x;
    int output_y;
    const char* composer;
    uint8_t composer_active;
    int composer_top;
    char history[TERM_HISTORY_CAPACITY][TERM_MAX_LINE];
    uint8_t history_count;
} TermState;

static const lt_Api* lt = 0;
static TermState state;

/* Per-VM module association.  Each VM that loads the module gets its own
   table; the table is recorded here and bound into `state.module` only when
   that VM acquires the terminal, so callbacks are rooted in the owning VM's
   table even when several VMs load the module. */
typedef struct
{
    lt_VM* vm;
    lt_Value module;
} TermModule;

static TermModule term_modules[TERM_MAX_MODULES];
static uint8_t term_module_count;

/**
 * Requires an exact number of arguments for the current call.
 * @param vm Virtual machine reporting the runtime error.
 * @param argc Number of arguments provided.
 * @param expected Required number of arguments.
 * @param message Error message to report when the counts differ.
 */
static void require_args(lt_VM* vm, uint8_t argc, uint8_t expected, const char* message)
{
    if (argc != expected) lt->runtime_error(vm, message);
}

/**
 * Requires an active terminal owned by the specified VM.
 * @param vm VM that must own the active terminal.
 */
static void require_active(lt_VM* vm)
{
    if (!state.active || state.vm != vm) lt->runtime_error(vm, "Call term.open before using the terminal!");
}

/**
 * Determines whether a value can be invoked.
 * @param value Value to inspect.
 * @return `1` if the value is a function, closure, native function, or class; `0` otherwise.
 */
static uint8_t is_callable(lt_Value value)
{
    return LT_IS_FUNCTION(value) || LT_IS_CLOSURE(value) || LT_IS_NATIVE(value) || LT_IS_CLASS(value);
}

/**
 * Duplicates a string using the C allocator.
 * @param text String to duplicate.
 * @return A newly allocated copy, or a null pointer when the text is null or allocation fails.
 */
static char* term_strdup(const char* text)
{
    size_t len;
    char* copy;
    if (!text) return 0;
    len = strlen(text);
    copy = (char*)malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, text, len + 1);
    return copy;
}

/**
 * Returns the module table registered for the specified VM.
 * @param vm VM whose module table is requested.
 * @return The VM's module table, or null when the VM has not loaded the module.
 */
static lt_Value term_module_for(lt_VM* vm)
{
    for (uint8_t i = 0; i < term_module_count; ++i)
    {
        if (term_modules[i].vm == vm) return term_modules[i].module;
    }
    return LT_VALUE_NULL;
}

/**
 * Maps a terminal key code to its event name.
 *
 * @param key Terminal key code.
 * @returns The corresponding event name, or a null pointer for an unmapped key.
 */
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

/**
 * Creates an event table for a terminal input code.
 * @param key Terminal key or resize code.
 * @return Event table containing the code and its corresponding event data.
 */
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

/**
 * Reads the next terminal key without waiting for input.
 *
 * @return The next key code, or ERR when no key is available.
 */
static int next_key(void)
{
    timeout(0);
    return getch();
}

/**
 * Configures the terminal output region above the reserved input row.
 */
static void configure_output_region(void)
{
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    (void)columns;
    if (rows > 1)
    {
        setscrreg(0, rows - 2);
        scrollok(stdscr, TRUE);
    }
}

/**
 * Advances the terminal output cursor to the beginning of the next line,
 * scrolling the output region when necessary.
 */
static void output_newline(void)
{
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    (void)columns;
    if (rows <= 1) return;
    if (state.output_y >= rows - 2)
    {
        move(rows - 2, 0);
        scroll(stdscr);
        state.output_y = rows - 2;
    }
    else state.output_y++;
    state.output_x = 0;
    move(state.output_y, state.output_x);
}

/**
 * Writes text to the terminal output region, handling carriage returns, newlines, and line wrapping.
 *
 * @param text Text to write.
 * @returns 1 if the text was written; 0 if the terminal is inactive, the text is null, or the screen is too small.
 */
uint8_t lt_term_write_output(const char* text)
{
    int rows, columns;
    if (!state.active || !text) return 0;
    getmaxyx(stdscr, rows, columns);
    if (rows <= 1 || columns <= 0) return 0;
    configure_output_region();
    if (state.output_y > rows - 2) state.output_y = rows - 2;
    if (state.output_x >= columns) state.output_x = 0;
    move(state.output_y, state.output_x);
    for (; *text; ++text)
    {
        if (*text == '\r') continue;
        if (*text == '\n') output_newline();
        else
        {
            addch((unsigned char)*text);
            state.output_x++;
            if (state.output_x >= columns) output_newline();
        }
    }
    refresh();
    return 1;
}

/**
 * Begins an empty multiline input composer.
 */
void lt_term_begin_composer(void)
{
    if (state.composer) free((void*)state.composer);
    state.composer = term_strdup("");
    state.composer_active = 1;
}

/**
 * Updates the text displayed in the active input composer.
 * @param text Replacement composer text; a null pointer clears the text.
 */
void lt_term_update_composer(const char* text)
{
    if (text)
    {
        char* copy = term_strdup(text);
        if (copy)
        {
            if (state.composer) free((void*)state.composer);
            state.composer = copy;
        }
    }
    else
    {
        if (state.composer) free((void*)state.composer);
        state.composer = 0;
    }
}

/**
 * Commits the active composer and clears its display area.
 */
void lt_term_commit_composer(void)
{
    if (!state.active || !state.composer_active) return;
    {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        (void)columns;
        for (int y = state.composer_top; y < rows; ++y)
        {
            move(y, 0);
            clrtoeol();
        }
    }
    if (state.composer) free((void*)state.composer);
    state.composer = 0;
    state.composer_active = 0;
    refresh();
}

/**
 * Processes at most one pending terminal input event for the owning VM.
 *
 * @param context Terminal state associated with the poll hook.
 * @return The poll status indicating whether work was performed or remains pending.
 */
static lt_PollResult term_poll_hook(lt_VM* vm, void* context)
{
    TermState* terminal = context;
    int key;
    if (!terminal->active || terminal->vm != vm) return LT_POLL_IDLE;
    key = next_key();
    if (key == ERR) return terminal->callback_set ? LT_POLL_PENDING : LT_POLL_IDLE;
    if (terminal->callback_set)
    {
        uint8_t saved_trap = vm->trap_errors;
        char* saved_trap_msg = vm->error_trap;
        vm->trap_errors = 1;
        vm->error_trap = 0;
        lt->push(vm, make_event(vm, key));
        uint16_t returns = lt->exec(vm, terminal->callback, 1);
        while (returns--) lt->pop(vm);
        char* error = vm->error_trap;
        vm->trap_errors = saved_trap;
        vm->error_trap = saved_trap_msg;
        if (error)
        {
            if (!saved_trap && vm->error) vm->error(vm, error);
            lt->free(vm, error);
            terminal->callback = LT_VALUE_NULL;
            terminal->callback_set = 0;
            if (state.module != LT_VALUE_NULL)
                lt->table_set(vm, state.module, lt->make_string(vm, "_callback"), LT_VALUE_NULL);
        }
    }
    return LT_POLL_WORK;
}

/**
 * Processes one pending terminal event for the virtual machine.
 *
 * @param vm Virtual machine associated with the terminal.
 * @returns `1` if terminal work was processed, `0` otherwise.
 */
uint8_t ltterm_update(lt_VM* vm)
{
    return term_poll_hook(vm, &state) == LT_POLL_WORK;
}

/**
 * Opens the terminal for the calling Little VM.
 *
 * @param vm Little VM requesting terminal ownership.
 * @param argc Number of arguments supplied to the native function.
 * @return Number of values pushed onto the VM stack.
 */
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
        state.module = term_module_for(vm);
        erase();
        configure_output_region();
        state.output_x = 0;
        state.output_y = 0;
        state.poll_hook = lt->add_poll_hook(vm, term_poll_hook, &state);
    }
    else if (state.vm != vm) lt->runtime_error(vm, "The terminal is already owned by another Little VM!");
    lt->push(vm, LT_VALUE_TRUE);
    return 1;
}

/**
 * Closes the terminal owned by the calling VM and clears its terminal state.
 * @param vm The VM that owns the terminal.
 * @param argc The number of arguments supplied to the function.
 */
static uint8_t term_close(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.close!");
    if (state.active && state.vm == vm)
    {
        lt->remove_poll_hook(vm, state.poll_hook);
        scrollok(stdscr, FALSE);
        endwin();
    }
    if (state.module != LT_VALUE_NULL && state.vm == vm)
        lt->table_set(vm, state.module, lt->make_string(vm, "_callback"), LT_VALUE_NULL);
    if (state.composer) free((void*)state.composer);
    state.composer = 0;
    state.module = LT_VALUE_NULL;
    state.active = 0;
    state.callback_set = 0;
    state.callback = LT_VALUE_NULL;
    state.vm = 0;
    state.poll_hook = 0;
    return 0;
}

/**
 * Determines whether the terminal is active for the calling VM.
 *
 * @returns `true` if the calling VM owns an active terminal, `false` otherwise.
 */
static uint8_t term_is_open(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.isOpen!");
    lt->push(vm, state.active && state.vm == vm ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

/**
 * Returns the dimensions of the active terminal.
 *
 * @returns A table containing the terminal width and height.
 */
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

/**
 * Clears the terminal screen and resets the output position.
 */
static uint8_t term_clear(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.clear!");
    require_active(vm);
    erase();
    configure_output_region();
    state.output_x = 0;
    state.output_y = 0;
    return 0;
}

/**
 * Clears the terminal row specified by the numeric argument.
 */
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

/**
 * Refreshes the terminal display.
 */
static uint8_t term_present(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.present!");
    require_active(vm);
    refresh();
    return 0;
}

/**
 * Moves the terminal cursor to the specified coordinates.
 * @param vm Virtual machine executing the operation.
 * @param argc Number of arguments supplied.
 */
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

/**
 * Sets the terminal cursor visibility.
 * @param vm Virtual machine invoking the function.
 * @param argc Number of arguments provided.
 */
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

/**
 * Writes text at the specified terminal coordinates.
 *
 * @param vm Virtual machine whose active terminal is used.
 * @param argc Number of arguments supplied to the function.
 */
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

/**
 * Emits a terminal alert sound.
 */
static uint8_t term_bell(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.bell!");
    require_active(vm);
    beep();
    return 0;
}

/**
 * Polls the terminal for one input event without blocking.
 * @returns A terminal event, or null when no input is available.
 */
static uint8_t term_poll(lt_VM* vm, uint8_t argc)
{
    int key;
    require_args(vm, argc, 0, "Expected no arguments to term.poll!");
    require_active(vm);
    key = next_key();
    lt->push(vm, key == ERR ? LT_VALUE_NULL : make_event(vm, key));
    return 1;
}

/**
 * Updates the terminal and reports whether work was performed.
 * @returns `true` if terminal input or callbacks required processing, `false` otherwise.
 */
static uint8_t term_update_native(lt_VM* vm, uint8_t argc)
{
    require_args(vm, argc, 0, "Expected no arguments to term.update!");
    require_active(vm);
    lt->push(vm, ltterm_update(vm) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

/**
 * Registers a callable callback for terminal events.
 *
 * @param argc Number of arguments supplied.
 * @returns The registered callback.
 * @throws Runtime error if the terminal is inactive, the argument count is invalid, or the callback is not callable.
 */
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

/**
 * Renders the input prompt, editable line, and active multiline composer, then refreshes the terminal display.
 * @param prompt Prompt displayed before the input line.
 * @param line Current editable input text.
 * @param cursor Cursor offset within the input line.
 */
static void draw_input(const char* prompt, const char* line, uint32_t cursor)
{
    const char* source;
    const char* end;
    int rows, columns;
    int available;
    int y;
    size_t prompt_len;
    size_t line_len;
    size_t offset;
    getmaxyx(stdscr, rows, columns);
    prompt_len = strlen(prompt);
    line_len = strlen(line);
    available = columns - (int)prompt_len;
    if (available < 1) available = 1;
    offset = 0;
    if (cursor >= (size_t)available) offset = cursor - (size_t)available + 1;
    y = rows - 1;
    if (state.composer_active)
    {
        uint32_t count = 1;
        for (source = state.composer ? state.composer : ""; *source; ++source)
            if (*source == '\n') count++;
        if (count + 1 < (uint32_t)rows) y = rows - (int)count - 1;
        else y = 0;
        state.composer_top = y;
    }
    for (int clear_y = y; clear_y < rows; ++clear_y)
    {
        move(clear_y, 0);
        clrtoeol();
    }
    if (state.composer_active)
    {
        move(y++, 0);
        addstr(">> \"\"\"");
        source = state.composer ? state.composer : "";
        while (*source && y < rows - 1)
        {
            end = strchr(source, '\n');
            move(y++, 0);
            addstr(">> ");
            if (end)
            {
                for (const char* item = source; item < end; ++item) addch((unsigned char)*item);
                source = end + 1;
            }
            else
            {
                addstr(source);
                break;
            }
        }
    }
    move(rows - 1, 0);
    addstr(prompt);
    mvaddnstr(rows - 1, (int)prompt_len, line + offset, available);
    move(rows - 1, (int)(prompt_len + (cursor - offset)));
    refresh();
}

/**
 * Stores a nonempty command line in history unless it duplicates the most recent entry.
 *
 * @param line Command line to store.
 */
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

/**
 * Reads an editable line of input from the terminal.
 *
 * @param prompt Text displayed before the input line.
 * @returns The submitted input string, or null if input is cancelled.
 */
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
    draw_input(prompt, line, cursor);
    timeout(-1);
    for (;;)
    {
        int key = getch();
        if (key == ERR || key == 4 || key == 3)
        {
            timeout(0);
            lt->push(vm, LT_VALUE_NULL);
            return 1;
        }
        if (key == '\r' || key == '\n' || key == KEY_ENTER)
        {
            timeout(0);
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
        else if (key >= 32 && key <= 255)
        {
            if (length + 1 < TERM_MAX_LINE)
            {
                memmove(line + cursor + 1, line + cursor, length - cursor + 1);
                line[cursor++] = (char)key; length++; history_index = -1;
            }
            else beep();
        }
        draw_input(prompt, line, cursor);
    }
}

/**
 * Shuts down the terminal and resets its state.
 */
void lt_term_shutdown(void)
{
    if (state.active) endwin();
    if (state.composer) free((void*)state.composer);
    state.composer = 0;
    memset(&state, 0, sizeof(state));
}

/**
 * Initializes and returns the terminal module.
 *
 * @param vm Virtual machine receiving the module.
 * @param api API interface used by the module.
 * @return The terminal module table, or null when the API version or size is invalid.
 */
LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    lt_Value term;
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api)) return LT_VALUE_NULL;
    lt = api;
    term = lt->make_table(vm);
    if (term_module_count < TERM_MAX_MODULES)
    {
        uint8_t found = 0;
        for (uint8_t i = 0; i < term_module_count; ++i)
        {
            if (term_modules[i].vm == vm)
            {
                term_modules[i].module = term;
                found = 1;
                break;
            }
        }
        if (!found)
        {
            term_modules[term_module_count].vm = vm;
            term_modules[term_module_count].module = term;
            term_module_count++;
        }
    }
#define TERM_FN(name, function) lt->table_set(vm, term, lt->make_string(vm, name), lt->make_native(vm, function))
    TERM_FN("open", term_open); TERM_FN("close", term_close); TERM_FN("isOpen", term_is_open);
    TERM_FN("size", term_size); TERM_FN("clear", term_clear); TERM_FN("clearLine", term_clear_line);
    TERM_FN("present", term_present); TERM_FN("move", term_move); TERM_FN("cursor", term_cursor);
    TERM_FN("write", term_write); TERM_FN("bell", term_bell); TERM_FN("poll", term_poll);
    TERM_FN("update", term_update_native); TERM_FN("onEvent", term_on_event); TERM_FN("readLine", term_read_line);
#undef TERM_FN
    return term;
}
