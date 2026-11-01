#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/little.h"
#include "src/little_host_internal.h"
#include "src/little_bundle.h"
#include "src/little_bundle_build.h"
#include "src/little_std.h"
#include "src/little_loadlib.h"
#include "src/little_async.h"
#include "src/cli_internal.h"

int cli_had_error = 0;

/**
 * Records a VM error and writes its message through the available output channel.
 *
 * @param vm VM associated with the error.
 * @param msg Error message to report.
 */
static void error(lt_VM* vm, const char* msg)
{
    (void)vm;
    cli_had_error = 1;
    if (ltstd_write_output("LT ERROR: "))
    {
        ltstd_write_output(msg);
        ltstd_write_output("\n");
        return;
    }
    /* A terminal program must never leave the caller in raw mode just because
       the VM rejected a script. */
    ltstd_close_term();
    printf("LT ERROR: %s\n", msg);
}

/**
 * Closes terminal resources and destroys the virtual machine.
 *
 * @param vm Virtual machine to destroy.
 */
static void destroy_vm(lt_VM* vm)
{
    ltstd_close_term();
    lt_destroy(vm);
}

/**
 * Prints command-line usage and option information to the specified stream.
 *
 * @param stream Stream to which the usage information is written.
 */
static void print_usage(FILE* stream)
{
    fprintf(stream,
        "Usage: little [options] (-i | -e SOURCE | FILE [ARG]...)\n"
        "       little --bundle ENTRY --include PATH... -o OUTPUT\n"
        "\n"
        "  -i, --interactive      Start an interactive terminal session.\n"
        "  -e SOURCE              Execute Little source supplied on the command line.\n"
        "  -I, --module-path DIR  Add a source-module search directory. May be repeated.\n"
        "  -L, --library-path DIR Add a native-library search directory. May be repeated.\n"
        "  --bundle FILE         Create a self-contained Little executable.\n"
        "  --include PATH        Add a file or directory to a bundle. May be repeated.\n"
        "  -o, --output FILE     Write a bundle to this executable path.\n"
        "  --config FILE          Load only this configuration file.\n"
        "  --no-config            Do not load default configuration files.\n"
        "  -v, --version          Show the Little API version used by this CLI.\n"
        "  -h, --help             Show this help.\n"
        "  --                     Stop option processing; remaining values are script arguments.\n");
}

int main(int argc, char** argv)
{
    const char* source = NULL;
    const char* module_name = NULL;
    const char* script_name = NULL;
    const char* bundle_entry_path = NULL;
    const char* bundle_output_path = NULL;
    char* file_source = NULL;
    char* executable = NULL;
    lt_Bundle* bundle = NULL;
    const char** module_paths = malloc((size_t)argc * sizeof(*module_paths));
    const char** bundle_include_paths = malloc((size_t)argc * sizeof(*bundle_include_paths));
    const char** library_paths = malloc((size_t)argc * sizeof(*library_paths));
    uint32_t module_path_count = 0;
    uint32_t library_path_count = 0;
    uint32_t bundle_include_count = 0;
    const char* config_path = NULL;
    int no_config = 0;
    int interactive = 0;
    int repl_echo = 0;
    int script_arg_count = 0;
    char** script_args = NULL;
    lt_VM* vm = NULL;
    int exit_code = 0;
    int bundled = lt_bundle_probe_self(argv[0]);

    if (bundled < 0)
    {
        fprintf(stderr, "ERROR: Failed to inspect the running executable bundle\n");
        exit_code = 1;
        goto cleanup;
    }

    if (!module_paths || !bundle_include_paths || !library_paths)
    {
        fprintf(stderr, "ERROR: Failed to allocate command-line options\n");
        exit_code = 1;
        goto cleanup;
    }

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(stdout);
            exit_code = 0;
            goto cleanup;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
        {
            printf("little API %d\n", LT_API_VERSION);
            exit_code = 0;
            goto cleanup;
        }
        else if (strcmp(argv[i], "-I") == 0 || strcmp(argv[i], "--module-path") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "ERROR: %s requires a directory\n", argv[i - 1]);
                exit_code = 2;
                goto cleanup;
            }
            module_paths[module_path_count++] = argv[i];
        }
        else if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--library-path") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "ERROR: %s requires a directory\n", argv[i - 1]);
                exit_code = 2;
                goto cleanup;
            }
            library_paths[library_path_count++] = argv[i];
        }
        else if (strcmp(argv[i], "--bundle") == 0)
        {
            if (bundle_entry_path || ++i >= argc)
            {
                fprintf(stderr, "ERROR: --bundle requires one entry file\n");
                exit_code = 2;
                goto cleanup;
            }
            bundle_entry_path = argv[i];
        }
        else if (strcmp(argv[i], "--include") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "ERROR: --include requires a file or directory\n");
                exit_code = 2;
                goto cleanup;
            }
            bundle_include_paths[bundle_include_count++] = argv[i];
        }
        else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0)
        {
            if (bundle_output_path || ++i >= argc)
            {
                fprintf(stderr, "ERROR: %s requires one output file\n", argv[i - 1]);
                exit_code = 2;
                goto cleanup;
            }
            bundle_output_path = argv[i];
        }
        else if (strcmp(argv[i], "--config") == 0)
        {
            if (config_path || ++i >= argc)
            {
                fprintf(stderr, "ERROR: --config requires one file\n");
                exit_code = 2;
                goto cleanup;
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
                exit_code = 2;
                goto cleanup;
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
                exit_code = 2;
                goto cleanup;
            }
            source = argv[i];
            module_name = "<command line>";
            script_name = "-e";
        }
        else if (strcmp(argv[i], "--") == 0)
        {
            if (bundled && !source && !interactive && !bundle_entry_path)
            {
                script_args = &argv[i + 1];
                script_arg_count = argc - i - 1;
                break;
            }
            if (++i >= argc || source || interactive)
            {
                print_usage(stderr);
                exit_code = 2;
                goto cleanup;
            }
            file_source = read_source_file(argv[i]);
            if (!file_source)
            {
                exit_code = 2;
                goto cleanup;
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
            exit_code = 2;
            goto cleanup;
        }
        else if (source || interactive)
        {
            print_usage(stderr);
            exit_code = 2;
            goto cleanup;
        }
        else if (bundled && !bundle_entry_path)
        {
            script_args = &argv[i];
            script_arg_count = argc - i;
            break;
        }
        else
        {
            file_source = read_source_file(argv[i]);
            if (!file_source)
            {
                exit_code = 2;
                goto cleanup;
            }
            source = file_source;
            module_name = "module";
            script_name = argv[i];
            script_args = &argv[i + 1];
            script_arg_count = argc - i - 1;
            break;
        }
    }

    if (bundle_entry_path || bundle_output_path || bundle_include_count > 0)
    {
        char bundle_error[512] = { 0 };
        char* runtime_path;
        int success;

        if (!bundle_entry_path || !bundle_output_path || source || interactive || module_path_count > 0 || library_path_count > 0)
        {
            fprintf(stderr, "ERROR: --bundle requires ENTRY and -o OUTPUT; it cannot run a script or use -I or -L\n");
            exit_code = 2;
            goto cleanup;
        }
        runtime_path = lt_executable_path(argv[0]);
        if (!runtime_path)
        {
            fprintf(stderr, "ERROR: Failed to locate the Little runtime\n");
            exit_code = 1;
            goto cleanup;
        }
        success = lt_bundle_create(runtime_path, bundle_output_path, bundle_entry_path, bundle_include_paths, bundle_include_count, bundle_error, sizeof(bundle_error));
        if (!success)
            fprintf(stderr, "ERROR: %s\n", bundle_error[0] ? bundle_error : "Failed to create bundle");
        else
            printf("Created bundle '%s'\n", bundle_output_path);
        free(runtime_path);
        exit_code = success ? 0 : 1;
        goto cleanup;
    }

    if (!source && !interactive && !bundled)
    {
        print_usage(stderr);
        exit_code = 2;
        goto cleanup;
    }

    vm = lt_open(malloc, free, error);
    if (!vm)
    {
        fprintf(stderr, "ERROR: Failed to initialize VM\n");
        exit_code = 1;
        goto cleanup;
    }
    cli_had_error = 0;
    executable = lt_executable_path(argv[0]);
    if (!executable)
    {
        fprintf(stderr, "ERROR: Failed to locate the Little runtime\n");
        exit_code = 1;
        goto cleanup;
    }

    if (bundled)
    {
        char bundle_error[512] = { 0 };
        size_t entry_size;
        bundle = lt_bundle_open_self(executable, bundle_error, sizeof(bundle_error));
        if (!bundle)
        {
            fprintf(stderr, "ERROR: %s\n", bundle_error[0] ? bundle_error : "Failed to open embedded bundle");
            exit_code = 1;
            goto cleanup;
        }
        if (!source && !interactive)
        {
            file_source = lt_bundle_read_entry(bundle, lt_bundle_entry(bundle), &entry_size, bundle_error, sizeof(bundle_error));
            if (!file_source)
            {
                fprintf(stderr, "ERROR: %s\n", bundle_error[0] ? bundle_error : "Failed to read bundle entry");
                exit_code = 1;
                goto cleanup;
            }
            source = file_source;
            module_name = lt_bundle_entry(bundle);
            script_name = module_name;
        }
        lt_add_module_loader(vm, lt_bundle_module_loader, bundle);
    }

    ltstd_open_all(vm);
    ltstd_open_loadlib(vm);
    ltstd_open_term(vm);
    ltasync_open_all(vm);

    if (!add_default_library_paths(vm, executable))
    {
        fprintf(stderr, "ERROR: Failed to configure default library paths\n");
        exit_code = 1;
        goto cleanup;
    }

    if (config_path)
    {
        if (!load_config(vm, config_path, 1, &repl_echo))
        {
            exit_code = 1;
            goto cleanup;
        }
    }
    else if (!no_config)
    {
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (home)
        {
            char* user_config = join_path(home, ".config/little.conf");
            if (!user_config || !load_config(vm, user_config, 0, &repl_echo))
            {
                if (!user_config)
                    fprintf(stderr, "ERROR: Failed to allocate configuration path\n");
                free(user_config);
                exit_code = 1;
                goto cleanup;
            }
            free(user_config);
        }
        char* executable_directory = parent_path(executable);
        char* local_config = executable_directory ? join_path(executable_directory, "little.conf") : NULL;
        free(executable_directory);
        if (!local_config || !load_config(vm, local_config, 0, &repl_echo))
        {
            if (!local_config)
                fprintf(stderr, "ERROR: Failed to allocate configuration path\n");
            free(local_config);
            exit_code = 1;
            goto cleanup;
        }
        free(local_config);
    }

    for (uint32_t i = 0; i < module_path_count; ++i)
    {
        if (!add_module_path(vm, module_paths[i]))
        {
            exit_code = 1;
            goto cleanup;
        }
    }
    for (uint32_t i = 0; i < library_path_count; ++i)
        ltstd_add_library_path(vm, library_paths[i]);

    set_script_args(vm, script_name, script_arg_count, script_args);

    if (interactive)
    {
        if (!run_repl(vm, repl_echo)) cli_had_error = 1;
    }
    else
    {
        uint32_t nreturn = lt_dostring(vm, source, module_name);
        if (!cli_had_error) lt_runloop(vm);

        while (!cli_had_error && nreturn-- > 0)
        {
            char* returned = ltstd_tostring(vm, lt_pop(vm));
            printf("Returned: %s\n", returned);
            free(returned);
        }
    }

    exit_code = cli_had_error ? 1 : 0;

cleanup:
    if (vm) destroy_vm(vm);
    lt_bundle_close(bundle);
    free(executable);
    free(file_source);
    free(module_paths);
    free(bundle_include_paths);
    free(library_paths);
    return exit_code;
}
