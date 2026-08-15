#pragma once

#include "little.h"
#include "little_loadlib.h"

void ltstd_open_all(lt_VM* vm);

char* ltstd_tostring(lt_VM* vm, lt_Value val);

void ltstd_open_io(lt_VM* vm);
void ltstd_open_term(lt_VM* vm);
void ltstd_close_term(void);
void ltstd_open_math(lt_VM* vm);
void ltstd_open_array(lt_VM* vm);
void ltstd_open_table(lt_VM* vm);
void ltstd_open_string(lt_VM* vm);
void ltstd_open_gc(lt_VM* vm);
