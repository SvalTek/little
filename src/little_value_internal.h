#pragma once

#include "little.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t hash;
	char* string;
	lt_Value value;
	uint32_t refcount;
	uint32_t len;
} lt_StringDedupEntry;

uint8_t faststrcmp(const char* a, uint64_t a_len, const char* b, uint64_t b_len);
void lt_reference_string_internal(lt_VM* vm, lt_Value value);

#if !defined(_WIN32)
int sprintf_s(char* restrict buffer, size_t bufsz, const char* restrict format, ...);
int strncpy_s(char* restrict dest, size_t destsz, const char* restrict src, size_t count);
#endif
