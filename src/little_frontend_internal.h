#pragma once

#include "little.h"

void lt_push_diagnostic_internal(lt_VM* vm, lt_Span span, uint16_t line, uint16_t col, uint8_t severity, const char* message);
void lt_syntax_error_internal(lt_VM* vm, const char* module, lt_Span span, uint16_t line, uint16_t col, const char* message);
void lt_tokenize_error_internal(lt_VM* vm, const char* module, uint16_t line, uint16_t col, lt_Span span, const char* message);
void lt_parse_error_internal(lt_VM* vm, const char* module, lt_Token* t, const char* message);
uint16_t lt_make_local_internal(lt_VM* vm, lt_Scope* scope, lt_Token* t);
uint8_t lt_is_captured_local_internal(lt_Scope* scope, uint32_t idx);
uint32_t lt_find_local_internal(lt_VM* vm, lt_Scope* scope, lt_Token* t);
lt_Token* lt_make_number_token_internal(lt_VM* vm, lt_Parser* p, double number, lt_Token* loc);
uint8_t lt_token_is_keyword_internal(lt_Token* token);

#define UPVAL_BIT 0x07000000
#define NOT_FOUND ((uint32_t)-1)
