#pragma once

#include "little.h"

extern int cli_had_error;

int run_repl(lt_VM* vm, int repl_echo);
int load_config(lt_VM* vm, const char* path, int required, int* repl_echo);
int add_default_library_paths(lt_VM* vm, const char* executable);
int add_module_path(lt_VM* vm, const char* path);
void set_script_args(lt_VM* vm, const char* script, int count, char** values);
char* read_source_file(const char* path);
char* join_path(const char* left, const char* right);
char* parent_path(const char* path);
