#include "little_std.h"

#include <stdio.h>
#include <time.h>

static ltstd_OutputWriter _ltstd_output_writer;

/**
 * Configures the callback used to write standard output.
 * @param writer Output callback, or NULL to disable the custom writer.
 */
void ltstd_set_output_writer(ltstd_OutputWriter writer)
{
    _ltstd_output_writer = writer;
}

/**
 * Writes text through the configured output writer.
 * @param text Text to write.
 * @return Nonzero if the configured writer reports success, or zero if no writer is configured.
 */
uint8_t ltstd_write_output(const char* text)
{
    return _ltstd_output_writer ? _ltstd_output_writer(text) : 0;
}

/**
 * Writes all arguments separated by spaces and followed by a newline.
 * @param vm Virtual machine containing the arguments.
 * @param argc Number of arguments to write.
 */
static uint8_t _lt_print(lt_VM* vm, uint8_t argc)
{
    for (int16_t i = argc - 1; i >= 0; --i)
    {
        char* str = ltstd_tostring(vm, vm->stack[vm->top - 1 - i]);
        if (!ltstd_write_output(str)) printf("%s", str);
        vm->free(str);

        if (i > 0 && !ltstd_write_output(" ")) printf(" ");
    }

    for (int16_t i = argc - 1; i >= 0; --i) lt_pop(vm);

    if (!ltstd_write_output("\n")) printf("\n");
    return 0;
}

static void _lt_write_values(lt_VM* vm, uint8_t argc)
{
    for (int16_t i = argc - 1; i >= 0; --i)
    {
        char* str = ltstd_tostring(vm, vm->stack[vm->top - 1 - i]);
        if (!ltstd_write_output(str)) printf("%s", str);
        vm->free(str);

        if (i > 0 && !ltstd_write_output(" ")) printf(" ");
    }

    for (int16_t i = argc - 1; i >= 0; --i) lt_pop(vm);
}

static uint8_t _lt_write(lt_VM* vm, uint8_t argc)
{
    _lt_write_values(vm, argc);
    return 0;
}

/**
 * Writes the supplied values separated by spaces, followed by a newline.
 */
static uint8_t _lt_writeline(lt_VM* vm, uint8_t argc)
{
    _lt_write_values(vm, argc);
    if (!ltstd_write_output("\n")) printf("\n");
    return 0;
}

static uint8_t _lt_clock(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to io.clock!");

    clock_t time = clock();
    double in_seconds = (double)time / (double)CLOCKS_PER_SEC;
    lt_push(vm, lt_make_number(in_seconds));

    return 1;
}

static uint8_t _lt_readline(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to io.readLine!");

    char buffer[1024];
    if (!fgets(buffer, sizeof(buffer), stdin))
    {
        lt_push(vm, LT_VALUE_NULL);
        return 1;
    }

    size_t len = 0;
    while (buffer[len] && buffer[len] != '\n' && buffer[len] != '\r') ++len;
    buffer[len] = 0;
    lt_push(vm, lt_make_string(vm, buffer));
    return 1;
}

static const char* _lt_pop_string_arg(lt_VM* vm, const char* message)
{
    lt_Value value = lt_pop(vm);
    if (!LT_IS_STRING(value)) lt_runtime_error(vm, message);
    return lt_get_string(vm, value);
}

static uint8_t _lt_readfile(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to io.readFile!");
    const char* path = _lt_pop_string_arg(vm, "Expected path argument to io.readFile to be string!");

    FILE* file = fopen(path, "rb");
    if (!file) lt_runtime_error(vm, "Unable to open file for reading!");

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    if (size < 0)
    {
        fclose(file);
        lt_runtime_error(vm, "Unable to read file size!");
    }
    fseek(file, 0, SEEK_SET);

    if ((unsigned long)size > UINT32_MAX - 1)
    {
        fclose(file);
        lt_runtime_error(vm, "File too large to read into memory!");
    }

    uint32_t alloc_size = (uint32_t)size;
    char* contents = vm->alloc(alloc_size + 1);
    if (!contents)
    {
        fclose(file);
        lt_runtime_error(vm, "Unable to allocate file buffer!");
    }
    size_t read = fread(contents, 1, (size_t)alloc_size, file);
    fclose(file);
    contents[read] = 0;

    lt_push(vm, lt_make_string(vm, contents));
    vm->free(contents);
    return 1;
}

static uint8_t _lt_writefile_mode(lt_VM* vm, uint8_t argc, const char* mode, const char* name)
{
    if (argc != 2) lt_runtime_error(vm, name);
    const char* contents = _lt_pop_string_arg(vm, "Expected contents argument to file write to be string!");
    const char* path = _lt_pop_string_arg(vm, "Expected path argument to file write to be string!");

    FILE* file = fopen(path, mode);
    if (!file) lt_runtime_error(vm, "Unable to open file for writing!");

    size_t len = 0;
    while (contents[len]) ++len;
    size_t written = fwrite(contents, 1, len, file);
    fclose(file);

    lt_push(vm, written == len ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_writefile(lt_VM* vm, uint8_t argc)
{
    return _lt_writefile_mode(vm, argc, "wb", "Expected two arguments to io.writeFile!");
}

static uint8_t _lt_appendfile(lt_VM* vm, uint8_t argc)
{
    return _lt_writefile_mode(vm, argc, "ab", "Expected two arguments to io.appendFile!");
}

static uint8_t _lt_appendline(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to io.appendLine!");
    const char* contents = _lt_pop_string_arg(vm, "Expected contents argument to io.appendLine to be string!");
    const char* path = _lt_pop_string_arg(vm, "Expected path argument to io.appendLine to be string!");

    FILE* file = fopen(path, "ab");
    if (!file) lt_runtime_error(vm, "Unable to open file for writing!");

    size_t len = 0;
    while (contents[len]) ++len;
    size_t written = fwrite(contents, 1, len, file);
    written += fwrite("\n", 1, 1, file);
    fclose(file);

    lt_push(vm, written == len + 1 ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

void ltstd_open_io(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    lt_table_set(vm, t, lt_make_string(vm, "print"), lt_make_native(vm, _lt_print));
    lt_table_set(vm, t, lt_make_string(vm, "write"), lt_make_native(vm, _lt_write));
    lt_table_set(vm, t, lt_make_string(vm, "writeLine"), lt_make_native(vm, _lt_writeline));
    lt_table_set(vm, t, lt_make_string(vm, "clock"), lt_make_native(vm, _lt_clock));
    lt_table_set(vm, t, lt_make_string(vm, "readLine"), lt_make_native(vm, _lt_readline));
    lt_table_set(vm, t, lt_make_string(vm, "readFile"), lt_make_native(vm, _lt_readfile));
    lt_table_set(vm, t, lt_make_string(vm, "writeFile"), lt_make_native(vm, _lt_writefile));
    lt_table_set(vm, t, lt_make_string(vm, "appendFile"), lt_make_native(vm, _lt_appendfile));
    lt_table_set(vm, t, lt_make_string(vm, "appendLine"), lt_make_native(vm, _lt_appendline));

    lt_table_set(vm, vm->global, lt_make_string(vm, "io"), t);
}
