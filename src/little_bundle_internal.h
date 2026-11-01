#pragma once

#include "little_bundle.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LT_BUNDLE_VERSION 1U
#define LT_BUNDLE_FOOTER_SIZE 32U
#define LT_BUNDLE_FOOTER_MAGIC "LTBNDL01"
#define LT_BUNDLE_ENTRY_PATH "__little__/entry"

void lt_bundle_set_error(char* error, size_t error_size, const char* format, ...);
char* lt_bundle_copy_string(const char* value);
char* lt_bundle_append_string(const char* left, const char* right);
char* lt_bundle_join_host_path(const char* left, const char* right);
char* lt_bundle_join_archive_path(const char* left, const char* right);
const char* lt_bundle_path_basename(const char* path);
int lt_bundle_seek_file(FILE* file, uint64_t offset);
uint64_t lt_bundle_tell_file(FILE* file);
int lt_bundle_file_size(FILE* file, uint64_t* size);
uint32_t lt_bundle_read_u32(const unsigned char* bytes);
uint64_t lt_bundle_read_u64(const unsigned char* bytes);
void lt_bundle_write_u32(unsigned char* bytes, uint32_t value);
void lt_bundle_write_u64(unsigned char* bytes, uint64_t value);
int lt_bundle_read_footer(const char* path, uint64_t* archive_offset, uint64_t* archive_size, char* error, size_t error_size);
