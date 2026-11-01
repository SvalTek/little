#pragma once

#include "little.h"

void ltstd_open_io(lt_VM* vm);
void ltstd_open_term(lt_VM* vm);
void ltstd_close_term(void);
void ltstd_open_math(lt_VM* vm);
void ltstd_open_number(lt_VM* vm);
void ltstd_open_bit(lt_VM* vm);
void ltstd_open_array(lt_VM* vm);
void ltstd_open_table(lt_VM* vm);
void ltstd_open_string(lt_VM* vm);
void ltstd_open_gc(lt_VM* vm);
void ltstd_open_json(lt_VM* vm);
void ltstd_open_web(lt_VM* vm);
void ltstd_open_ws(lt_VM* vm);

uint8_t ltstd_import_internal(lt_VM* vm, uint8_t argc);
uint8_t ltstd_module_add_path_internal(lt_VM* vm, uint8_t argc);
uint8_t ltstd_module_clear_paths_internal(lt_VM* vm, uint8_t argc);
lt_ModuleLoaderResult ltstd_file_module_loader_internal(lt_VM* vm, const char* requested, char** source, char** module_name, void* userdata);

uint8_t lt_json_parse(lt_VM* vm, const char* text, lt_Value* out);
lt_Value lt_json_stringify(lt_VM* vm, lt_Value value);
