#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/little.h"
#include "src/little_std.h"
#include "src/little_async.h"

static int had_error = 0;

static void error(lt_VM* vm, const char* msg)
{
    (void)vm;
    had_error = 1;
    printf("LT ERROR: %s\n", msg);
}

static void print_usage(FILE* stream)
{
    fprintf(stream,
        "Usage: little [--help] [--version] [-I DIRECTORY]... (-e SOURCE | FILE)\n"
        "\n"
        "  -I DIRECTORY Add a source-module search directory. May be repeated.\n"
        "  -e SOURCE   Execute Little source supplied on the command line.\n"
        "  --help      Show this help.\n"
        "  --version   Show the Little API version used by this CLI.\n");
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
    char* file_source = NULL;
    const char** module_paths = malloc((size_t)argc * sizeof(*module_paths));
    uint32_t module_path_count = 0;

    if (argc == 2 && strcmp(argv[1], "--help") == 0)
    {
        print_usage(stdout);
        free(module_paths);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0)
    {
        printf("little API %d\n", LT_API_VERSION);
        free(module_paths);
        return 0;
    }

    if (!module_paths)
    {
        fprintf(stderr, "ERROR: Failed to allocate command-line options\n");
        return 1;
    }

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-I") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "ERROR: -I requires a directory\n");
                free(module_paths);
                return 2;
            }
            module_paths[module_path_count++] = argv[i];
        }
        else if (strcmp(argv[i], "-e") == 0)
        {
            if (source || ++i >= argc)
            {
                print_usage(stderr);
                free(module_paths);
                return 2;
            }
            source = argv[i];
            module_name = "<command line>";
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "ERROR: Unknown option '%s'\n", argv[i]);
            free(module_paths);
            return 2;
        }
        else if (source)
        {
            print_usage(stderr);
            free(module_paths);
            return 2;
        }
        else
        {
            file_source = read_source_file(argv[i]);
            if (!file_source)
            {
                free(module_paths);
                return 2;
            }
            source = file_source;
            module_name = "module";
        }
    }

    if (!source)
    {
        print_usage(stderr);
        free(module_paths);
        return 2;
    }

    // Init VM and run program
    lt_VM* vm = lt_open(malloc, free, error);
    if (!vm)
    {
        fprintf(stderr, "ERROR: Failed to initialize VM\n");
        free(file_source);
        free(module_paths);
        return 1;
    }
    ltstd_open_all(vm);
    ltstd_open_loadlib(vm);
    ltasync_open_all(vm);

    had_error = 0;
    for (uint32_t i = 0; i < module_path_count; ++i)
    {
        if (!add_module_path(vm, module_paths[i]))
        {
            lt_destroy(vm);
            free(file_source);
            free(module_paths);
            return 1;
        }
    }

    uint32_t nreturn = lt_dostring(vm, source, module_name);
    if (!had_error) lt_runloop(vm);

    while (!had_error && nreturn-- > 0)
    {
        char* returned = ltstd_tostring(vm, lt_pop(vm));
        printf("Returned: %s\n", returned);
        free(returned);
    }

    lt_destroy(vm);
    free(file_source);
    free(module_paths);

    return had_error ? 1 : 0;
}
