#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "src/little.h"
#include "src/little_std.h"
#include "src/little_async.h"

static int had_error = 0;

static char* copy_string(const char* value);

static void error(lt_VM* vm, const char* msg)
{
    (void)vm;
    had_error = 1;
    /* A terminal program must never leave the caller in raw mode just because
       the VM rejected a script. */
    ltstd_close_term();
    printf("LT ERROR: %s\n", msg);
}

static void destroy_vm(lt_VM* vm)
{
    ltstd_close_term();
    lt_destroy(vm);
}

static void print_usage(FILE* stream)
{
    fprintf(stream,
        "Usage: little [options] (-i | -e SOURCE | FILE [ARG]...)\n"
        "\n"
        "  -i, --interactive      Start an interactive terminal session.\n"
        "  -e SOURCE              Execute Little source supplied on the command line.\n"
        "  -I, --module-path DIR  Add a source-module search directory. May be repeated.\n"
        "  -L, --library-path DIR Add a native-library search directory. May be repeated.\n"
        "  --config FILE          Load only this configuration file.\n"
        "  --no-config            Do not load default configuration files.\n"
        "  -v, --version          Show the Little API version used by this CLI.\n"
        "  -h, --help             Show this help.\n"
        "  --                     Stop option processing; remaining values are script arguments.\n");
}

static int call_term(lt_VM* vm, const char* name, const char* prompt, lt_Value* result)
{
    lt_Value term = lt_table_get(vm, vm->global, lt_make_string(vm, "term"));
    lt_Value function = lt_table_get(vm, term, lt_make_string(vm, name));
    uint8_t argc = prompt ? 1 : 0;
    if (prompt) lt_push(vm, lt_make_string(vm, prompt));
    uint16_t returns = lt_exec(vm, function, argc);
    if (had_error) return 0;

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

static int run_repl(lt_VM* vm)
{
    printf("little API %d interactive mode (Ctrl-C or Ctrl-D to exit)\n", LT_API_VERSION);
    for (;;)
    {
        lt_Value line = LT_VALUE_NULL;
        char* source;
        uint32_t nreturn;

        if (!call_term(vm, "open", 0, 0)) return 0;
        if (!call_term(vm, "readLine", "little> ", &line)) return 0;
        if (!call_term(vm, "close", 0, 0)) return 0;
        if (LT_IS_NULL(line)) return 1;

        /* readLine's return value is no longer on the VM stack. Keep a host
           copy so compiling the entry cannot collect the string underneath us. */
        source = copy_string(lt_get_string(vm, line));
        if (!source)
        {
            fprintf(stderr, "ERROR: Failed to allocate interactive input\n");
            return 0;
        }
        nreturn = lt_dostring(vm, source, "<interactive>");
        free(source);
        if (had_error)
        {
            /* A bad entry must not end the interactive session. */
            had_error = 0;
            continue;
        }

        while (nreturn-- > 0)
        {
            char* returned = ltstd_tostring(vm, lt_pop(vm));
            printf("%s\n", returned);
            free(returned);
        }
    }
}

static char* copy_string(const char* value)
{
    size_t length = strlen(value);
    char* copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

static char* join_path(const char* left, const char* right)
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

static char* parent_path(const char* path)
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

static int is_absolute_path(const char* path)
{
    if (path[0] == '/' || path[0] == '\\') return 1;
    return path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static char* path_from_directory(const char* directory, const char* path)
{
    return is_absolute_path(path) ? copy_string(path) : join_path(directory, path);
}

static char* executable_path(const char* argv0)
{
#ifdef _WIN32
    char buffer[32768];
    DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)sizeof(buffer));
    if (length > 0 && length < sizeof(buffer)) return copy_string(buffer);
#elif defined(__linux__)
    char buffer[32768];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0 && length < (ssize_t)sizeof(buffer))
    {
        buffer[length] = 0;
        return copy_string(buffer);
    }
#endif
    return copy_string(argv0);
}

static char* trim(char* value)
{
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t' || value[length - 1] == '\r' || value[length - 1] == '\n'))
        value[--length] = 0;
    return value;
}

static int add_module_path(lt_VM* vm, const char* path)
{
    lt_Value module = lt_table_get(vm, vm->global, lt_make_string(vm, "module"));
    lt_Value add_path = lt_table_get(vm, module, lt_make_string(vm, "addPath"));
    lt_push(vm, lt_make_string(vm, path));
    uint16_t returns = lt_exec(vm, add_path, 1);
    while (returns-- > 0) lt_pop(vm);
    return !had_error;
}

static int load_config(lt_VM* vm, const char* path, int required)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        if (!required) return 1;
        fprintf(stderr, "ERROR: Failed to open configuration '%s'\n", path);
        return 0;
    }

    char* directory = parent_path(path);
    if (!directory)
    {
        fclose(file);
        fprintf(stderr, "ERROR: Failed to allocate configuration path\n");
        return 0;
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
            free(directory);
            fclose(file);
            return 0;
        }
        *equals = 0;
        char* key = trim(entry);
        char* value = trim(equals + 1);
        if (!*value)
        {
            fprintf(stderr, "ERROR: %s:%u: Path cannot be empty\n", path, line_number);
            free(directory);
            fclose(file);
            return 0;
        }

        char* resolved = path_from_directory(directory, value);
        if (!resolved)
        {
            fprintf(stderr, "ERROR: Failed to allocate configuration path\n");
            free(directory);
            fclose(file);
            return 0;
        }

        if (strcmp(key, "module_path") == 0)
        {
            if (!add_module_path(vm, resolved))
            {
                free(resolved);
                free(directory);
                fclose(file);
                return 0;
            }
        }
        else if (strcmp(key, "library_path") == 0)
            ltstd_add_library_path(vm, resolved);
        else
        {
            fprintf(stderr, "ERROR: %s:%u: Unknown setting '%s'\n", path, line_number, key);
            free(resolved);
            free(directory);
            fclose(file);
            return 0;
        }
        free(resolved);
    }

    if (ferror(file))
    {
        fprintf(stderr, "ERROR: Failed to read configuration '%s'\n", path);
        free(directory);
        fclose(file);
        return 0;
    }

    free(directory);
    fclose(file);
    return 1;
}

static int add_default_library_paths(lt_VM* vm, const char* executable)
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

static void set_script_args(lt_VM* vm, const char* script, int count, char** values)
{
    lt_Value args = lt_make_array(vm);
    lt_array_push(vm, args, lt_make_string(vm, script));
    for (int i = 0; i < count; ++i)
        lt_array_push(vm, args, lt_make_string(vm, values[i]));
    lt_table_set(vm, vm->global, lt_make_string(vm, "arg"), args);
}

static char* read_source_file(const char* path)
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

int main(int argc, char** argv)
{
    const char* source = NULL;
    const char* module_name = NULL;
    const char* script_name = NULL;
    char* file_source = NULL;
    const char** module_paths = malloc((size_t)argc * sizeof(*module_paths));
    const char** library_paths = malloc((size_t)argc * sizeof(*library_paths));
    uint32_t module_path_count = 0;
    uint32_t library_path_count = 0;
    const char* config_path = NULL;
    int no_config = 0;
    int interactive = 0;
    int script_arg_count = 0;
    char** script_args = NULL;

    if (!module_paths || !library_paths)
    {
        fprintf(stderr, "ERROR: Failed to allocate command-line options\n");
        free(module_paths);
        free(library_paths);
        return 1;
    }

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(stdout);
            free(module_paths);
            free(library_paths);
            return 0;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
        {
            printf("little API %d\n", LT_API_VERSION);
            free(module_paths);
            free(library_paths);
            return 0;
        }
        else if (strcmp(argv[i], "-I") == 0 || strcmp(argv[i], "--module-path") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "ERROR: %s requires a directory\n", argv[i - 1]);
                free(module_paths);
                free(library_paths);
                return 2;
            }
            module_paths[module_path_count++] = argv[i];
        }
        else if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--library-path") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "ERROR: %s requires a directory\n", argv[i - 1]);
                free(module_paths);
                free(library_paths);
                return 2;
            }
            library_paths[library_path_count++] = argv[i];
        }
        else if (strcmp(argv[i], "--config") == 0)
        {
            if (config_path || ++i >= argc)
            {
                fprintf(stderr, "ERROR: --config requires one file\n");
                free(module_paths);
                free(library_paths);
                return 2;
            }
            config_path = argv[i];
        }
        else if (strcmp(argv[i], "--no-config") == 0)
        {
            no_config = 1;
        }
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
        {
            if (source || interactive)
            {
                print_usage(stderr);
                free(module_paths);
                free(library_paths);
                return 2;
            }
            interactive = 1;
            module_name = "<interactive>";
            script_name = "<interactive>";
        }
        else if (strcmp(argv[i], "-e") == 0)
        {
            if (source || interactive || ++i >= argc)
            {
                print_usage(stderr);
                free(module_paths);
                free(library_paths);
                return 2;
            }
            source = argv[i];
            module_name = "<command line>";
            script_name = "-e";
        }
        else if (strcmp(argv[i], "--") == 0)
        {
            if (++i >= argc || source || interactive)
            {
                print_usage(stderr);
                free(module_paths);
                free(library_paths);
                return 2;
            }
            file_source = read_source_file(argv[i]);
            if (!file_source)
            {
                free(module_paths);
                free(library_paths);
                return 2;
            }
            source = file_source;
            module_name = "module";
            script_name = argv[i];
            script_args = &argv[i + 1];
            script_arg_count = argc - i - 1;
            break;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "ERROR: Unknown option '%s'\n", argv[i]);
            free(module_paths);
            free(library_paths);
            return 2;
        }
        else if (source || interactive)
        {
            print_usage(stderr);
            free(module_paths);
            free(library_paths);
            return 2;
        }
        else
        {
            file_source = read_source_file(argv[i]);
            if (!file_source)
            {
                free(module_paths);
                free(library_paths);
                return 2;
            }
            source = file_source;
            module_name = "module";
            script_name = argv[i];
            script_args = &argv[i + 1];
            script_arg_count = argc - i - 1;
            break;
        }
    }

    if (!source && !interactive)
    {
        print_usage(stderr);
        free(module_paths);
        free(library_paths);
        return 2;
    }

    // Init VM and run program
    lt_VM* vm = lt_open(malloc, free, error);
    if (!vm)
    {
        fprintf(stderr, "ERROR: Failed to initialize VM\n");
        free(file_source);
        free(module_paths);
        free(library_paths);
        return 1;
    }
    ltstd_open_all(vm);
    ltstd_open_loadlib(vm);
    ltstd_open_term(vm);
    ltasync_open_all(vm);

    had_error = 0;
    char* executable = executable_path(argv[0]);
    if (!executable || !add_default_library_paths(vm, executable))
    {
        fprintf(stderr, "ERROR: Failed to configure default library paths\n");
        free(executable);
        destroy_vm(vm);
        free(file_source);
        free(module_paths);
        free(library_paths);
        return 1;
    }

    if (config_path)
    {
        if (!load_config(vm, config_path, 1))
        {
            free(executable);
            destroy_vm(vm);
            free(file_source);
            free(module_paths);
            free(library_paths);
            return 1;
        }
    }
    else if (!no_config)
    {
        const char* home = getenv("HOME");
        if (home)
        {
            char* user_config = join_path(home, ".config/little.conf");
            if (!user_config || !load_config(vm, user_config, 0))
            {
                free(user_config);
                free(executable);
                destroy_vm(vm);
                free(file_source);
                free(module_paths);
                free(library_paths);
                return 1;
            }
            free(user_config);
        }
        char* executable_directory = parent_path(executable);
        char* local_config = executable_directory ? join_path(executable_directory, "little.conf") : NULL;
        free(executable_directory);
        if (!local_config || !load_config(vm, local_config, 0))
        {
            free(local_config);
            free(executable);
            destroy_vm(vm);
            free(file_source);
            free(module_paths);
            free(library_paths);
            return 1;
        }
        free(local_config);
    }
    free(executable);

    for (uint32_t i = 0; i < module_path_count; ++i)
    {
        if (!add_module_path(vm, module_paths[i]))
        {
            destroy_vm(vm);
            free(file_source);
            free(module_paths);
            free(library_paths);
            return 1;
        }
    }
    for (uint32_t i = 0; i < library_path_count; ++i)
        ltstd_add_library_path(vm, library_paths[i]);

    set_script_args(vm, script_name, script_arg_count, script_args);

    if (interactive)
    {
        if (!run_repl(vm)) had_error = 1;
    }
    else
    {
        uint32_t nreturn = lt_dostring(vm, source, module_name);
        if (!had_error) lt_runloop(vm);

        while (!had_error && nreturn-- > 0)
        {
            char* returned = ltstd_tostring(vm, lt_pop(vm));
            printf("Returned: %s\n", returned);
            free(returned);
        }
    }

    destroy_vm(vm);
    free(file_source);
    free(module_paths);
    free(library_paths);

    return had_error ? 1 : 0;
}
