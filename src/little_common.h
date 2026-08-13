#pragma once

#include "little.h"

char* lt_common_copy_string(lt_VM* vm, const char* string);
char* lt_common_make_suffixed_path(lt_VM* vm, const char* path, const char* suffix);
char* lt_common_join_path(lt_VM* vm, const char* root, const char* path);
char* lt_common_expand_path_pattern(lt_VM* vm, const char* pattern, const char* requested);
uint8_t lt_common_has_suffix(const char* path, const char* suffix);
uint8_t lt_common_file_exists(const char* path);
lt_Value lt_common_module_paths(lt_VM* vm, uint8_t create);
