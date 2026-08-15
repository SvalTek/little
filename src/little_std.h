#pragma once

#include "little.h"
#include "little_loadlib.h"

void ltstd_open_all(lt_VM* vm);

char* ltstd_tostring(lt_VM* vm, lt_Value val);

void ltstd_open_io(lt_VM* vm);
void ltstd_open_term(lt_VM* vm);
void ltstd_close_term(void);
typedef uint8_t(*ltstd_OutputWriter)(const char* text);
void ltstd_set_output_writer(ltstd_OutputWriter writer);
uint8_t ltstd_write_output(const char* text);
void ltstd_term_begin_composer(void);
void ltstd_term_update_composer(const char* text);
void ltstd_term_commit_composer(void);
void ltstd_open_math(lt_VM* vm);
void ltstd_open_array(lt_VM* vm);
void ltstd_open_table(lt_VM* vm);
void ltstd_open_string(lt_VM* vm);
void ltstd_open_gc(lt_VM* vm);
