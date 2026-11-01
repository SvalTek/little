#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include "little_bundle.h"
#include "little_bundle_internal.h"

#include "little_common.h"
#include "little.h"
#include "little_host_internal.h"
#include "miniz.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define LT_BUNDLE_VERSION 1U
#define LT_BUNDLE_FOOTER_SIZE 32U
#define LT_BUNDLE_FOOTER_MAGIC "LTBNDL01"
#define LT_BUNDLE_ENTRY_PATH "__little__/entry"

struct lt_Bundle {
    mz_zip_archive archive;
    char* entry;
};

typedef struct {
    char* archive_name;
    char* source_path;
} BundleEntry;

typedef struct {
    BundleEntry* values;
    size_t length;
    size_t capacity;
} BundleEntryList;

void lt_bundle_set_error(char* error, size_t error_size, const char* format, ...)
{
    va_list args;
    if (!error || error_size == 0) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

char* lt_bundle_copy_string(const char* value)
{
    size_t length = strlen(value);
    char* copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

char* lt_bundle_append_string(const char* left, const char* right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    char* result = malloc(left_length + right_length + 1);
    if (!result) return NULL;
    memcpy(result, left, left_length);
    memcpy(result + left_length, right, right_length + 1);
    return result;
}

char* lt_bundle_join_host_path(const char* left, const char* right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    int separator = left_length > 0 && left[left_length - 1] != '/' && left[left_length - 1] != '\\';
    char* result = malloc(left_length + (size_t)separator + right_length + 1);
    if (!result) return NULL;
    memcpy(result, left, left_length);
    if (separator) result[left_length++] = '/';
    memcpy(result + left_length, right, right_length + 1);
    return result;
}

char* lt_bundle_join_archive_path(const char* left, const char* right)
{
    if (!*left) return lt_bundle_copy_string(right);
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    char* result = malloc(left_length + right_length + 2);
    if (!result) return NULL;
    memcpy(result, left, left_length);
    result[left_length] = '/';
    memcpy(result + left_length + 1, right, right_length + 1);
    return result;
}

const char* lt_bundle_path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* separator = slash;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    return separator ? separator + 1 : path;
}

int lt_bundle_seek_file(FILE* file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

uint64_t lt_bundle_tell_file(FILE* file)
{
#ifdef _WIN32
    __int64 offset = _ftelli64(file);
    return offset < 0 ? UINT64_MAX : (uint64_t)offset;
#else
    off_t offset = ftello(file);
    return offset < 0 ? UINT64_MAX : (uint64_t)offset;
#endif
}

int lt_bundle_file_size(FILE* file, uint64_t* size)
{
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) return 0;
#else
    if (fseeko(file, 0, SEEK_END) != 0) return 0;
#endif
    *size = lt_bundle_tell_file(file);
    return *size != UINT64_MAX;
}

uint32_t lt_bundle_read_u32(const unsigned char* bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

uint64_t lt_bundle_read_u64(const unsigned char* bytes)
{
    uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index)
        value |= (uint64_t)bytes[index] << (index * 8);
    return value;
}

void lt_bundle_write_u32(unsigned char* bytes, uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
        bytes[index] = (unsigned char)(value >> (index * 8));
}

void lt_bundle_write_u64(unsigned char* bytes, uint64_t value)
{
    for (unsigned int index = 0; index < 8; ++index)
        bytes[index] = (unsigned char)(value >> (index * 8));
}

int lt_bundle_read_footer(const char* path, uint64_t* archive_offset, uint64_t* archive_size, char* error, size_t error_size)
{
    FILE* file = fopen(path, "rb");
    unsigned char footer[LT_BUNDLE_FOOTER_SIZE];
    uint64_t size;

    if (!file) {
        lt_bundle_set_error(error, error_size, "Failed to open executable '%s'", path);
        return -1;
    }
    if (!lt_bundle_file_size(file, &size) || size < LT_BUNDLE_FOOTER_SIZE) {
        fclose(file);
        return 0;
    }
    if (!lt_bundle_seek_file(file, size - LT_BUNDLE_FOOTER_SIZE) || fread(footer, 1, sizeof(footer), file) != sizeof(footer)) {
        fclose(file);
        lt_bundle_set_error(error, error_size, "Failed to read bundle footer from '%s'", path);
        return -1;
    }
    fclose(file);

    if (memcmp(footer, LT_BUNDLE_FOOTER_MAGIC, 8) != 0) return 0;
    if (lt_bundle_read_u32(footer + 24) != LT_BUNDLE_VERSION) {
        lt_bundle_set_error(error, error_size, "Unsupported Little bundle version in '%s'", path);
        return -1;
    }

    *archive_offset = lt_bundle_read_u64(footer + 8);
    *archive_size = lt_bundle_read_u64(footer + 16);
    if (*archive_offset > size - LT_BUNDLE_FOOTER_SIZE || *archive_size > size - LT_BUNDLE_FOOTER_SIZE - *archive_offset || *archive_size == 0) {
        lt_bundle_set_error(error, error_size, "Invalid Little bundle archive range in '%s'", path);
        return -1;
    }
    return 1;
}

int lt_bundle_probe_self(const char* argv0)
{
    char* path = lt_executable_path(argv0);
    uint64_t archive_offset;
    uint64_t archive_size;
    int result;
    char error[256];

    if (!path) return -1;
    result = lt_bundle_read_footer(path, &archive_offset, &archive_size, error, sizeof(error));
    free(path);
    return result;
}

static int zip_file_index(mz_zip_archive* archive, const char* name, char* error, size_t error_size)
{
    int index = mz_zip_reader_locate_file(archive, name, NULL, 0);
    if (index < 0) return -1;
    if ((uint32_t)index >= archive->m_total_files) {
        lt_bundle_set_error(error, error_size, "Invalid bundled module index for '%s'", name);
        return -2;
    }
    return index;
}

static char* read_zip_entry(mz_zip_archive* archive, const char* name, size_t* size, char* error, size_t error_size)
{
    mz_zip_archive_file_stat stat;
    int index = zip_file_index(archive, name, error, error_size);
    char* data;

    if (index < 0) return NULL;
    if (!mz_zip_reader_file_stat(archive, (mz_uint)index, &stat)) {
        lt_bundle_set_error(error, error_size, "Failed to inspect bundled entry '%s'", name);
        return NULL;
    }
    if (stat.m_uncomp_size > (uint64_t)SIZE_MAX - 1) {
        lt_bundle_set_error(error, error_size, "Bundled entry '%s' is too large", name);
        return NULL;
    }

    *size = (size_t)stat.m_uncomp_size;
    data = malloc(*size + 1);
    if (!data) {
        lt_bundle_set_error(error, error_size, "Failed to allocate bundled entry '%s'", name);
        return NULL;
    }
    if (*size > 0 && !mz_zip_reader_extract_to_mem(archive, (mz_uint)index, data, *size, 0)) {
        free(data);
        lt_bundle_set_error(error, error_size, "Failed to read bundled entry '%s'", name);
        return NULL;
    }
    data[*size] = 0;
    return data;
}

lt_Bundle* lt_bundle_open_self(const char* path, char* error, size_t error_size)
{
    uint64_t archive_offset;
    uint64_t archive_size;
    lt_Bundle* bundle;
    size_t entry_size;

    if (lt_bundle_read_footer(path, &archive_offset, &archive_size, error, error_size) != 1) {
        if (!error || !*error) lt_bundle_set_error(error, error_size, "Executable '%s' is not a Little bundle", path);
        return NULL;
    }

    bundle = calloc(1, sizeof(*bundle));
    if (!bundle) {
        lt_bundle_set_error(error, error_size, "Failed to allocate bundle state");
        return NULL;
    }
    mz_zip_zero_struct(&bundle->archive);
    if (!mz_zip_reader_init_file_v2(&bundle->archive, path, 0, archive_offset, archive_size)) {
        lt_bundle_set_error(error, error_size, "Failed to open embedded bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&bundle->archive)));
        free(bundle);
        return NULL;
    }

    bundle->entry = read_zip_entry(&bundle->archive, LT_BUNDLE_ENTRY_PATH, &entry_size, error, error_size);
    if (!bundle->entry || entry_size == 0) {
        if (!error || !*error) lt_bundle_set_error(error, error_size, "Bundle is missing '%s'", LT_BUNDLE_ENTRY_PATH);
        lt_bundle_close(bundle);
        return NULL;
    }
    return bundle;
}

void lt_bundle_close(lt_Bundle* bundle)
{
    if (!bundle) return;
    mz_zip_reader_end(&bundle->archive);
    free(bundle->entry);
    free(bundle);
}

const char* lt_bundle_entry(const lt_Bundle* bundle)
{
    return bundle ? bundle->entry : NULL;
}

char* lt_bundle_read_entry(lt_Bundle* bundle, const char* name, size_t* size, char* error, size_t error_size)
{
    if (!bundle) {
        lt_bundle_set_error(error, error_size, "Bundle is not open");
        return NULL;
    }
    return read_zip_entry(&bundle->archive, name, size, error, error_size);
}

static char* make_module_candidate(const char* requested, int initializer)
{
    size_t length = strlen(requested);
    int has_extension = lt_common_has_little_extension(requested);
    const char* suffix = initializer ? "/init.little" : (has_extension ? "" : ".little");
    size_t suffix_length = strlen(suffix);
    char* candidate = malloc(length + suffix_length + 1);
    if (!candidate) return NULL;
    for (size_t index = 0; index < length; ++index)
        candidate[index] = requested[index] == '\\' ? '/' : requested[index];
    if (initializer && length > 0 && candidate[length - 1] == '/')
        memcpy(candidate + length, suffix + 1, suffix_length);
    else
        memcpy(candidate + length, suffix, suffix_length + 1);
    return candidate;
}

static int read_zip_entry_vm(lt_VM* vm, mz_zip_archive* archive, const char* name, char** source)
{
    mz_zip_archive_file_stat stat;
    char error[256] = { 0 };
    int index = zip_file_index(archive, name, error, sizeof(error));
    if (index == -2)
        lt_runtime_error(vm, error[0] ? error : "Invalid bundled module index!");
    if (index < 0) return 0;
    if (!mz_zip_reader_file_stat(archive, (mz_uint)index, &stat))
        lt_runtime_error(vm, "Failed to inspect bundled module!");
    if (stat.m_uncomp_size > (uint64_t)SIZE_MAX - 1)
        lt_runtime_error(vm, "Bundled module is too large!");

    *source = vm->alloc((size_t)stat.m_uncomp_size + 1);
    if (stat.m_uncomp_size > 0 && !mz_zip_reader_extract_to_mem(archive, (mz_uint)index, *source, (size_t)stat.m_uncomp_size, 0)) {
        vm->free(*source);
        *source = NULL;
        lt_runtime_error(vm, "Failed to read bundled module!");
    }
    (*source)[stat.m_uncomp_size] = 0;
    return 1;
}

lt_ModuleLoaderResult lt_bundle_module_loader(lt_VM* vm, const char* requested, char** source, char** module_name, void* userdata)
{
    lt_Bundle* bundle = userdata;
    char* candidate;

    candidate = make_module_candidate(requested, 0);
    if (!candidate) lt_runtime_error(vm, "Failed to allocate bundled module path!");
    if (read_zip_entry_vm(vm, &bundle->archive, candidate, source)) {
        *module_name = vm->alloc(strlen(candidate) + 1);
        strcpy(*module_name, candidate);
        free(candidate);
        return LT_MODULE_LOADER_FOUND;
    }
    free(candidate);

    candidate = make_module_candidate(requested, 1);
    if (!candidate) lt_runtime_error(vm, "Failed to allocate bundled module path!");
    if (read_zip_entry_vm(vm, &bundle->archive, candidate, source)) {
        *module_name = vm->alloc(strlen(candidate) + 1);
        strcpy(*module_name, candidate);
        free(candidate);
        return LT_MODULE_LOADER_FOUND;
    }
    free(candidate);
    return LT_MODULE_LOADER_NOT_FOUND;
}
