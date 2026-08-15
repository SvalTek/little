#pragma once

#include "little.h"

void ltstd_open_loadlib(lt_VM* vm);

/* Adds a native-library search root for loadLibrary. Source-module search
   paths remain separate and are configured through module.addPath. */
void ltstd_add_library_path(lt_VM* vm, const char* path);
