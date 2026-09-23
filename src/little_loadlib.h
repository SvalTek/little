#pragma once

#include "little.h"

void ltstd_open_loadlib(lt_VM* vm);
void ltstd_add_library_path(lt_VM* vm, const char* path);
const lt_Api* ltstd_native_api(void);
