#include "cli_internal.h"
#include "little_std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LT_REPL_DRAIN_LIMIT 1000

static int call_term(lt_VM* vm, const char* name, const char* prompt, lt_Value* result)
{
    lt_Value term = lt_table_get(vm, vm->global, lt_make_string(vm, "term"));
    lt_Value function = lt_table_get(vm, term, lt_make_string(vm, name));
    uint8_t argc = prompt ? 1 : 0;
    if (prompt) lt_push(vm, lt_make_string(vm, prompt));
    uint16_t returns = lt_exec(vm, function, argc);
    if (cli_had_error) return 0;

    if (result)
    {
        *result = returns ? lt_pop(vm) : LT_VALUE_NULL;
        while (returns > 1)
        {
            lt_pop(vm);
            returns--;
        }
    }
    else while (returns-- > 0) lt_pop(vm);
    return 1;
}

/**
 * Appends a line and a trailing newline to a dynamically sized source buffer.
 * @param source Buffer receiving the appended line.
 * @param length Current length of the source buffer.
 * @param capacity Allocated capacity of the source buffer.
 * @param line Line to append.
 * @return 1 on success, 0 if the buffer cannot be resized.
 */
static int append_repl_line(char** source, size_t* length, size_t* capacity, const char* line)
{
    size_t line_length = strlen(line);
    size_t required = *length + line_length + 2;
    if (required > *capacity)
    {
        size_t next_capacity = *capacity ? *capacity * 2 : 256;
        while (next_capacity < required) next_capacity *= 2;
        char* next = realloc(*source, next_capacity);
        if (!next) return 0;
        *source = next;
        *capacity = next_capacity;
    }
    memcpy(*source + *length, line, line_length);
    *length += line_length;
    (*source)[(*length)++] = '\n';
    (*source)[*length] = 0;
    return 1;
}

/**
 * Echoes a REPL input line with its prompt.
 * @param line Input line to display.
 * @param length Number of characters to display.
 */
static void echo_repl_line(const char* line, size_t length)
{
    char character[2] = { 0, 0 };
    ltstd_write_output(">> ");
    for (size_t i = 0; i < length; ++i)
    {
        character[0] = line[i];
        ltstd_write_output(character);
    }
    ltstd_write_output("\n");
}

/**
 * Echoes REPL source, optionally enclosing multiline input in triple quotes.
 * @param source Source text to echo.
 * @param multiline Whether to include multiline delimiters.
 */
static void echo_repl_source(const char* source, int multiline)
{
    const char* line = source;
    if (multiline) echo_repl_line("\"\"\"", 3);
    while (*line)
    {
        const char* end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        echo_repl_line(line, length);
        if (!end) break;
        line = end + 1;
    }
    if (multiline) echo_repl_line("\"\"\"", 3);
}

/**
 * Executes source entered in the interactive session and displays its returned values.
 *
 * @param vm Virtual machine used to execute the source.
 * @param source Source code to execute.
 * @return 1 after processing the source, including when execution reports an error.
 */
static int run_repl_source(lt_VM* vm, const char* source)
{
    uint32_t nreturn = lt_dostring(vm, source, "<interactive>");
    if (cli_had_error)
    {
        /* A bad entry must not end the interactive session. */
        cli_had_error = 0;
        return 1;
    }
    /* Drain immediately-runnable async work (promises, timers, hooks) so
       deferred callbacks progress between REPL entries without blocking
       the prompt. The bound stops a repeating immediate timer from
       hanging the session. */
    for (uint32_t steps = 0; steps < LT_REPL_DRAIN_LIMIT && !cli_had_error && lt_poll_now(vm); ++steps) {}
    if (cli_had_error)
        cli_had_error = 0; /* drain errors were already reported; keep the session alive */
    while (nreturn-- > 0)
    {
        char* returned = ltstd_tostring(vm, lt_pop(vm));
        ltstd_write_output(returned);
        ltstd_write_output("\n");
        free(returned);
    }
    return 1;
}

/**
 * Runs the interactive read-eval-print loop, supporting single-line and multiline input.
 *
 * @param vm Virtual machine used to execute interactive input.
 * @param repl_echo Whether entered source is echoed.
 * @return 1 if the session ends normally, 0 if terminal or input processing fails.
 */
int run_repl(lt_VM* vm, int repl_echo)
{
    char banner[128];
    char* captured = 0;
    size_t captured_length = 0;
    size_t captured_capacity = 0;
    int capturing = 0;
    int success = 0;

    if (!call_term(vm, "open", 0, 0)) goto done;
    snprintf(banner, sizeof(banner), "little API %d interactive mode (Ctrl-C or Ctrl-D to exit)\n", LT_API_VERSION);
    ltstd_write_output(banner);
    ltstd_write_output("Enter \"\"\" to begin a multiline capture.\n");
    for (;;)
    {
        lt_Value line = LT_VALUE_NULL;
        const char* text;
        if (!call_term(vm, "readLine", ">> ", &line)) goto done;
        if (LT_IS_NULL(line)) { success = 1; goto done; }
        text = lt_get_string(vm, line);

        if (capturing)
        {
            if (strcmp(text, "\"\"\"") == 0)
            {
                capturing = 0;
                ltstd_term_commit_composer();
                if (repl_echo) echo_repl_source(captured ? captured : "", 1);
                if (!run_repl_source(vm, captured ? captured : "")) goto done;
                captured_length = 0;
                if (captured) captured[0] = 0;
            }
            else if (!append_repl_line(&captured, &captured_length, &captured_capacity, text))
            {
                ltstd_write_output("ERROR: Failed to allocate multiline input.\n");
                goto done;
            }
            else ltstd_term_update_composer(captured);
            continue;
        }

        if (strcmp(text, "\"\"\"") == 0)
        {
            captured_length = 0;
            if (captured) captured[0] = 0;
            capturing = 1;
            ltstd_term_begin_composer();
            continue;
        }
        if (*text)
        {
            if (repl_echo) echo_repl_source(text, 0);
            if (!run_repl_source(vm, text)) goto done;
        }
    }

done:
    free(captured);
    call_term(vm, "close", 0, 0);
    return success;
}
