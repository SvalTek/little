#include "cli_internal.h"
#include "little_loadlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* copy_string(const char* value)
{
    size_t length = strlen(value);
    char* copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

/**
 * Joins two path components with a separator when needed.
 *
 * @param left The first path component.
 * @param right The second path component.
 * @return A newly allocated combined path, or NULL if allocation fails.
 */
char* join_path(const char* left, const char* right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    int separator = left_length > 0 && left[left_length - 1] != '/' && left[left_length - 1] != '\\';
    char* path = malloc(left_length + (size_t)separator + right_length + 1);
    if (!path) return NULL;
    memcpy(path, left, left_length);
    if (separator) path[left_length++] = '/';
    memcpy(path + left_length, right, right_length + 1);
    return path;
}

/**
 * Returns the directory component of a path.
 * @param path Path whose parent directory is requested.
 * @return A newly allocated parent path, or "." when no separator is present. Returns NULL if allocation fails.
 */
char* parent_path(const char* path)
{
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* separator = slash;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    if (!separator) return copy_string(".");
    size_t length = (size_t)(separator - path);
    if (length == 0) length = 1;
    char* parent = malloc(length + 1);
    if (!parent) return NULL;
    memcpy(parent, path, length);
    parent[length] = 0;
    return parent;
}

/**
 * Determines whether a path is absolute.
 *
 * @param path Path to inspect.
 * @return 1 if the path is absolute, 0 otherwise.
 */
static int is_absolute_path(const char* path)
{
    if (path[0] == '/' || path[0] == '\\') return 1;
    return path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

/**
 * Resolves a path relative to a directory when it is not absolute.
 * @param directory Base directory for relative paths.
 * @param path Path to resolve.
 * @returns A newly allocated absolute or directory-relative path.
 */
static char* path_from_directory(const char* directory, const char* path)
{
    return is_absolute_path(path) ? copy_string(path) : join_path(directory, path);
}

/**
 * Removes leading and trailing whitespace from a mutable string.
 * @param value String to trim.
 * @returns The trimmed string.
 */
static char* trim(char* value)
{
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t' || value[length - 1] == '\r' || value[length - 1] == '\n'))
        value[--length] = 0;
    return value;
}

/**
 * Adds a directory to the module search path.
 * @param path Directory to add to the module search path.
 * @return `true` if the path is added successfully, `false` if a VM error occurs.
 */
int add_module_path(lt_VM* vm, const char* path)
{
    lt_Value module = lt_table_get(vm, vm->global, lt_make_string(vm, "module"));
    lt_Value add_path = lt_table_get(vm, module, lt_make_string(vm, "addPath"));
    lt_push(vm, lt_make_string(vm, path));
    uint16_t returns = lt_exec(vm, add_path, 1);
    while (returns-- > 0) lt_pop(vm);
    return !cli_had_error;
}

/**
 * Loads module, library, and REPL echo settings from a configuration file.
 *
 * @param vm VM to configure.
 * @param path Configuration file path.
 * @param required Whether failure to open the file is an error.
 * @param repl_echo Receives the configured REPL echo setting.
 * @return 1 if the configuration is loaded successfully or an optional file is absent, 0 otherwise.
 */
int load_config(lt_VM* vm, const char* path, int required, int* repl_echo)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        if (!required) return 1;
        fprintf(stderr, "ERROR: Failed to open configuration '%s'\n", path);
        return 0;
    }

    char* directory = parent_path(path);
    char* resolved = 0;
    if (!directory)
    {
        fprintf(stderr, "ERROR: Failed to allocate configuration path\n");
        goto fail;
    }

    char line[4096];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), file))
    {
        line_number++;
        char* entry = trim(line);
        if (!*entry || *entry == '#' || *entry == ';') continue;

        char* equals = strchr(entry, '=');
        if (!equals)
        {
            fprintf(stderr, "ERROR: %s:%u: Expected KEY = PATH\n", path, line_number);
            goto fail;
        }
        *equals = 0;
        char* key = trim(entry);
        char* value = trim(equals + 1);
        if (!*value)
        {
            fprintf(stderr, "ERROR: %s:%u: Path cannot be empty\n", path, line_number);
            goto fail;
        }

        if (strcmp(key, "repl_echo") == 0)
        {
            if (strcmp(value, "true") == 0) *repl_echo = 1;
            else if (strcmp(value, "false") == 0) *repl_echo = 0;
            else
            {
                fprintf(stderr, "ERROR: %s:%u: repl_echo must be true or false\n", path, line_number);
                goto fail;
            }
            continue;
        }

        resolved = path_from_directory(directory, value);
        if (!resolved)
        {
            fprintf(stderr, "ERROR: Failed to allocate configuration path\n");
            goto fail;
        }

        if (strcmp(key, "module_path") == 0)
        {
            if (!add_module_path(vm, resolved))
                goto fail;
        }
        else if (strcmp(key, "library_path") == 0)
            ltstd_add_library_path(vm, resolved);
        else
        {
            fprintf(stderr, "ERROR: %s:%u: Unknown setting '%s'\n", path, line_number, key);
            goto fail;
        }
        free(resolved);
        resolved = 0;
    }

    if (ferror(file))
    {
        fprintf(stderr, "ERROR: Failed to read configuration '%s'\n", path);
        goto fail;
    }

    free(directory);
    fclose(file);
    return 1;

fail:
    free(resolved);
    free(directory);
    fclose(file);
    return 0;
}

/**
 * Adds the default library directories derived from the executable path.
 *
 * @param vm VM to configure.
 * @param executable Path to the executable.
 * @return 1 if both library paths are created and registered, 0 on allocation failure.
 */
int add_default_library_paths(lt_VM* vm, const char* executable)
{
    char* directory = parent_path(executable);
    if (!directory) return 0;

    char* portable = join_path(directory, "libs");
    char* prefix = parent_path(directory);
    char* library = prefix ? join_path(prefix, "lib/little") : NULL;
    free(prefix);
    free(directory);
    if (!portable || !library)
    {
        free(portable);
        free(library);
        return 0;
    }

    ltstd_add_library_path(vm, portable);
    ltstd_add_library_path(vm, library);
    free(portable);
    free(library);
    return 1;
}

/**
 * Sets the global `arg` array with the script name followed by its arguments.
 *
 * @param script Script name stored as the first element.
 * @param count Number of arguments in `values`.
 * @param values Script argument values.
 */
void set_script_args(lt_VM* vm, const char* script, int count, char** values)
{
    lt_Value args = lt_make_array(vm);
    lt_array_push(vm, args, lt_make_string(vm, script));
    for (int i = 0; i < count; ++i)
        lt_array_push(vm, args, lt_make_string(vm, values[i]));
    lt_table_set(vm, vm->global, lt_make_string(vm, "arg"), args);
}

/**
 * Reads a source file into a null-terminated string.
 * @param path Path to the source file.
 * @return Newly allocated file contents, or NULL if the file cannot be read or memory allocation fails.
 */
char* read_source_file(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "ERROR: Failed to open '%s'\n", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fprintf(stderr, "ERROR: Failed to seek '%s'\n", path);
        fclose(file);
        return NULL;
    }

    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "ERROR: Failed to read '%s'\n", path);
        fclose(file);
        return NULL;
    }

    size_t size = (size_t)length;
    char* source = malloc(size + 1);
    if (!source)
    {
        fprintf(stderr, "ERROR: Failed to allocate %zu bytes for '%s'\n", size + 1, path);
        fclose(file);
        return NULL;
    }

    size_t read = fread(source, 1, size, file);
    if (read != size || ferror(file))
    {
        fprintf(stderr, "ERROR: Failed to read '%s'\n", path);
        free(source);
        fclose(file);
        return NULL;
    }

    source[size] = '\0';
    fclose(file);
    return source;
}
