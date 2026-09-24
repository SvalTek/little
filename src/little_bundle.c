#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include "little_bundle.h"

#include "little_common.h"
#include "little_internal.h"
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

static void set_error(char* error, size_t error_size, const char* format, ...)
{
    va_list args;
    if (!error || error_size == 0) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static char* copy_string(const char* value)
{
    size_t length = strlen(value);
    char* copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

static char* append_string(const char* left, const char* right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    char* result = malloc(left_length + right_length + 1);
    if (!result) return NULL;
    memcpy(result, left, left_length);
    memcpy(result + left_length, right, right_length + 1);
    return result;
}

static char* join_host_path(const char* left, const char* right)
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

static char* join_archive_path(const char* left, const char* right)
{
    if (!*left) return copy_string(right);
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    char* result = malloc(left_length + right_length + 2);
    if (!result) return NULL;
    memcpy(result, left, left_length);
    result[left_length] = '/';
    memcpy(result + left_length + 1, right, right_length + 1);
    return result;
}

static const char* path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* separator = slash;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    return separator ? separator + 1 : path;
}

static int seek_file(FILE* file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static uint64_t tell_file(FILE* file)
{
#ifdef _WIN32
    __int64 offset = _ftelli64(file);
    return offset < 0 ? UINT64_MAX : (uint64_t)offset;
#else
    off_t offset = ftello(file);
    return offset < 0 ? UINT64_MAX : (uint64_t)offset;
#endif
}

static int file_size(FILE* file, uint64_t* size)
{
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) return 0;
#else
    if (fseeko(file, 0, SEEK_END) != 0) return 0;
#endif
    *size = tell_file(file);
    return *size != UINT64_MAX;
}

static uint32_t read_u32(const unsigned char* bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const unsigned char* bytes)
{
    uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index)
        value |= (uint64_t)bytes[index] << (index * 8);
    return value;
}

static void write_u32(unsigned char* bytes, uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
        bytes[index] = (unsigned char)(value >> (index * 8));
}

static void write_u64(unsigned char* bytes, uint64_t value)
{
    for (unsigned int index = 0; index < 8; ++index)
        bytes[index] = (unsigned char)(value >> (index * 8));
}

static int read_footer(const char* path, uint64_t* archive_offset, uint64_t* archive_size, char* error, size_t error_size)
{
    FILE* file = fopen(path, "rb");
    unsigned char footer[LT_BUNDLE_FOOTER_SIZE];
    uint64_t size;

    if (!file) {
        set_error(error, error_size, "Failed to open executable '%s'", path);
        return -1;
    }
    if (!file_size(file, &size) || size < LT_BUNDLE_FOOTER_SIZE) {
        fclose(file);
        return 0;
    }
    if (!seek_file(file, size - LT_BUNDLE_FOOTER_SIZE) || fread(footer, 1, sizeof(footer), file) != sizeof(footer)) {
        fclose(file);
        set_error(error, error_size, "Failed to read bundle footer from '%s'", path);
        return -1;
    }
    fclose(file);

    if (memcmp(footer, LT_BUNDLE_FOOTER_MAGIC, 8) != 0) return 0;
    if (read_u32(footer + 24) != LT_BUNDLE_VERSION) {
        set_error(error, error_size, "Unsupported Little bundle version in '%s'", path);
        return -1;
    }

    *archive_offset = read_u64(footer + 8);
    *archive_size = read_u64(footer + 16);
    if (*archive_offset > size - LT_BUNDLE_FOOTER_SIZE || *archive_size > size - LT_BUNDLE_FOOTER_SIZE - *archive_offset || *archive_size == 0) {
        set_error(error, error_size, "Invalid Little bundle archive range in '%s'", path);
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
    result = read_footer(path, &archive_offset, &archive_size, error, sizeof(error));
    free(path);
    return result;
}

static int zip_file_index(mz_zip_archive* archive, const char* name, char* error, size_t error_size)
{
    int index = mz_zip_reader_locate_file(archive, name, NULL, 0);
    if (index < 0) return -1;
    if ((uint32_t)index >= archive->m_total_files) {
        set_error(error, error_size, "Invalid bundled module index for '%s'", name);
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
        set_error(error, error_size, "Failed to inspect bundled entry '%s'", name);
        return NULL;
    }
    if (stat.m_uncomp_size > (uint64_t)SIZE_MAX - 1) {
        set_error(error, error_size, "Bundled entry '%s' is too large", name);
        return NULL;
    }

    *size = (size_t)stat.m_uncomp_size;
    data = malloc(*size + 1);
    if (!data) {
        set_error(error, error_size, "Failed to allocate bundled entry '%s'", name);
        return NULL;
    }
    if (*size > 0 && !mz_zip_reader_extract_to_mem(archive, (mz_uint)index, data, *size, 0)) {
        free(data);
        set_error(error, error_size, "Failed to read bundled entry '%s'", name);
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

    if (read_footer(path, &archive_offset, &archive_size, error, error_size) != 1) {
        if (!error || !*error) set_error(error, error_size, "Executable '%s' is not a Little bundle", path);
        return NULL;
    }

    bundle = calloc(1, sizeof(*bundle));
    if (!bundle) {
        set_error(error, error_size, "Failed to allocate bundle state");
        return NULL;
    }
    mz_zip_zero_struct(&bundle->archive);
    if (!mz_zip_reader_init_file_v2(&bundle->archive, path, 0, archive_offset, archive_size)) {
        set_error(error, error_size, "Failed to open embedded bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&bundle->archive)));
        free(bundle);
        return NULL;
    }

    bundle->entry = read_zip_entry(&bundle->archive, LT_BUNDLE_ENTRY_PATH, &entry_size, error, error_size);
    if (!bundle->entry || entry_size == 0) {
        if (!error || !*error) set_error(error, error_size, "Bundle is missing '%s'", LT_BUNDLE_ENTRY_PATH);
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
        set_error(error, error_size, "Bundle is not open");
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
    int index = zip_file_index(archive, name, NULL, 0);
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

static int path_kind(const char* path)
{
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return -1;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
    struct stat stat_buffer;
    if (lstat(path, &stat_buffer) != 0) return 0;
    if (S_ISLNK(stat_buffer.st_mode)) return -1;
    if (S_ISDIR(stat_buffer.st_mode)) return 2;
    return S_ISREG(stat_buffer.st_mode) ? 1 : -1;
#endif
}

static char* canonical_path(const char* path)
{
#ifdef _WIN32
    char buffer[32768];
    DWORD length = GetFullPathNameA(path, (DWORD)sizeof(buffer), buffer, NULL);
    if (length == 0 || length >= sizeof(buffer)) return NULL;
    return copy_string(buffer);
#else
    return realpath(path, NULL);
#endif
}

static int paths_equal(const char* left, const char* right)
{
    char* left_canonical = canonical_path(left);
    char* right_canonical = canonical_path(right);
    int equal = 0;

    if (left_canonical && right_canonical)
    {
#ifdef _WIN32
        equal = _stricmp(left_canonical, right_canonical) == 0;
#else
        equal = strcmp(left_canonical, right_canonical) == 0;
#endif
    }
    free(left_canonical);
    free(right_canonical);
    return equal;
}

static void free_entry_list(BundleEntryList* list)
{
    for (size_t index = 0; index < list->length; ++index) {
        free(list->values[index].archive_name);
        free(list->values[index].source_path);
    }
    free(list->values);
    memset(list, 0, sizeof(*list));
}

static int add_entry(BundleEntryList* list, const char* archive_name, const char* source_path, char* error, size_t error_size)
{
    char* canonical = canonical_path(source_path);
    if (!canonical) {
        set_error(error, error_size, "Failed to resolve included file '%s'", source_path);
        return 0;
    }
    if (strncmp(archive_name, "__little__/", 11) == 0 || strcmp(archive_name, "__little__") == 0) {
        free(canonical);
        set_error(error, error_size, "Archive path '%s' is reserved", archive_name);
        return 0;
    }

    for (size_t index = 0; index < list->length; ++index) {
        if (strcmp(list->values[index].archive_name, archive_name) != 0) continue;
        if (strcmp(list->values[index].source_path, canonical) == 0) {
            free(canonical);
            return 1;
        }
        free(canonical);
        set_error(error, error_size, "Bundle path collision: %s", archive_name);
        return 0;
    }

    if (list->length == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        BundleEntry* values = realloc(list->values, capacity * sizeof(*values));
        if (!values) {
            free(canonical);
            set_error(error, error_size, "Failed to allocate bundle file list");
            return 0;
        }
        list->values = values;
        list->capacity = capacity;
    }
    list->values[list->length].archive_name = copy_string(archive_name);
    list->values[list->length].source_path = canonical;
    if (!list->values[list->length].archive_name) {
        free(canonical);
        set_error(error, error_size, "Failed to allocate bundle archive path");
        return 0;
    }
    list->length++;
    return 1;
}

static int collect_directory(const char* directory, const char* archive_prefix, BundleEntryList* list, char* error, size_t error_size);

static int collect_path(const char* path, const char* archive_name, BundleEntryList* list, char* error, size_t error_size)
{
    int kind = path_kind(path);
    if (kind == 1) return add_entry(list, archive_name, path, error, error_size);
    if (kind == 2) return collect_directory(path, archive_name, list, error, error_size);
    if (kind == -1) set_error(error, error_size, "Symlinks and reparse points are not allowed in bundles: %s", path);
    else set_error(error, error_size, "Included path does not exist: %s", path);
    return 0;
}

static int collect_directory(const char* directory, const char* archive_prefix, BundleEntryList* list, char* error, size_t error_size)
{
#ifdef _WIN32
    char* pattern = join_host_path(directory, "*");
    WIN32_FIND_DATAA data;
    HANDLE handle;
    if (!pattern) {
        set_error(error, error_size, "Failed to allocate directory path");
        return 0;
    }
    handle = FindFirstFileA(pattern, &data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, error_size, "Failed to read directory '%s'", directory);
        return 0;
    }
    do {
        char* child_path;
        char* child_archive;
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        child_path = join_host_path(directory, data.cFileName);
        child_archive = join_archive_path(archive_prefix, data.cFileName);
        if (!child_path || !child_archive) {
            free(child_path);
            free(child_archive);
            FindClose(handle);
            set_error(error, error_size, "Failed to allocate bundle path");
            return 0;
        }
        if (!collect_path(child_path, child_archive, list, error, error_size)) {
            free(child_path);
            free(child_archive);
            FindClose(handle);
            return 0;
        }
        free(child_path);
        free(child_archive);
    } while (FindNextFileA(handle, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(handle);
        set_error(error, error_size, "Failed to read directory '%s'", directory);
        return 0;
    }
    FindClose(handle);
    return 1;
#else
    DIR* handle = opendir(directory);
    struct dirent* data;
    if (!handle) {
        set_error(error, error_size, "Failed to read directory '%s'", directory);
        return 0;
    }
    while ((data = readdir(handle)) != NULL) {
        char* child_path;
        char* child_archive;
        if (strcmp(data->d_name, ".") == 0 || strcmp(data->d_name, "..") == 0) continue;
        child_path = join_host_path(directory, data->d_name);
        child_archive = join_archive_path(archive_prefix, data->d_name);
        if (!child_path || !child_archive) {
            free(child_path);
            free(child_archive);
            closedir(handle);
            set_error(error, error_size, "Failed to allocate bundle path");
            return 0;
        }
        if (!collect_path(child_path, child_archive, list, error, error_size)) {
            free(child_path);
            free(child_archive);
            closedir(handle);
            return 0;
        }
        free(child_path);
        free(child_archive);
    }
    if (closedir(handle) != 0) {
        set_error(error, error_size, "Failed to close directory '%s'", directory);
        return 0;
    }
    return 1;
#endif
}

static int collect_input(const char* path, BundleEntryList* list, char* error, size_t error_size)
{
    int kind = path_kind(path);
    if (kind == 2) return collect_directory(path, "", list, error, error_size);
    if (kind == 1) return add_entry(list, path_basename(path), path, error, error_size);
    if (kind == -1) set_error(error, error_size, "Symlinks and reparse points are not allowed in bundles: %s", path);
    else set_error(error, error_size, "Included path does not exist: %s", path);
    return 0;
}

static int copy_prefix(const char* source_path, const char* destination_path, uint64_t size, char* error, size_t error_size)
{
    FILE* source = fopen(source_path, "rb");
    FILE* destination = fopen(destination_path, "wb");
    unsigned char buffer[65536];
    uint64_t remaining = size;

    if (!source || !destination) {
        if (source) fclose(source);
        if (destination) fclose(destination);
        set_error(error, error_size, "Failed to open runtime while creating bundle");
        return 0;
    }
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (fread(buffer, 1, chunk, source) != chunk || fwrite(buffer, 1, chunk, destination) != chunk) {
            fclose(source);
            fclose(destination);
            set_error(error, error_size, "Failed to copy Little runtime into bundle");
            return 0;
        }
        remaining -= chunk;
    }
    fclose(source);
    fclose(destination);
    return 1;
}

static int append_file_contents(const char* source_path, const char* destination_path, char* error, size_t error_size)
{
    FILE* source = fopen(source_path, "rb");
    FILE* destination = fopen(destination_path, "ab");
    unsigned char buffer[65536];
    size_t read_count;

    if (!source || !destination) {
        if (source) fclose(source);
        if (destination) fclose(destination);
        set_error(error, error_size, "Failed to append bundle archive");
        return 0;
    }
    while ((read_count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, read_count, destination) != read_count) {
            fclose(source);
            fclose(destination);
            set_error(error, error_size, "Failed to append bundle archive");
            return 0;
        }
    }
    if (ferror(source)) {
        fclose(source);
        fclose(destination);
        set_error(error, error_size, "Failed to read bundle archive");
        return 0;
    }
    fclose(source);
    fclose(destination);
    return 1;
}

static int append_footer(const char* path, uint64_t archive_offset, uint64_t archive_size, char* error, size_t error_size)
{
    unsigned char footer[LT_BUNDLE_FOOTER_SIZE] = { 0 };
    FILE* file = fopen(path, "ab");
    if (!file) {
        set_error(error, error_size, "Failed to append bundle footer to '%s'", path);
        return 0;
    }
    memcpy(footer, LT_BUNDLE_FOOTER_MAGIC, 8);
    write_u64(footer + 8, archive_offset);
    write_u64(footer + 16, archive_size);
    write_u32(footer + 24, LT_BUNDLE_VERSION);
    write_u32(footer + 28, 0);
    if (fwrite(footer, 1, sizeof(footer), file) != sizeof(footer)) {
        fclose(file);
        set_error(error, error_size, "Failed to write bundle footer to '%s'", path);
        return 0;
    }
    fclose(file);
    return 1;
}

int lt_bundle_create(
    const char* runtime_path,
    const char* output_path,
    const char* entry_path,
    const char* const* include_paths,
    uint32_t include_count,
    char* error,
    size_t error_size
)
{
    BundleEntryList entries = { 0 };
    uint64_t runtime_size;
    uint64_t archive_offset;
    uint64_t archive_size;
    int footer_status;
    char* entry_name = NULL;
    char* archive_path = NULL;
    char* temporary_path = NULL;
    mz_zip_archive writer;
    int writer_open = 0;
    int result = 0;

    if (!runtime_path) {
        set_error(error, error_size, "Runtime executable path is required");
        return 0;
    }
    if (!output_path) {
        set_error(error, error_size, "Bundle output path is required");
        return 0;
    }
    if (!entry_path) {
        set_error(error, error_size, "Bundle entry is required");
        return 0;
    }
    if (paths_equal(runtime_path, output_path)) {
        set_error(error, error_size, "Bundle output must differ from the runtime executable");
        return 0;
    }
    if (path_kind(entry_path) != 1) {
        set_error(error, error_size, "Bundle entry must be a regular file: %s", entry_path);
        return 0;
    }
    {
        FILE* runtime = fopen(runtime_path, "rb");
        if (!runtime || !file_size(runtime, &runtime_size)) {
            if (runtime) fclose(runtime);
            set_error(error, error_size, "Failed to read runtime executable '%s'", runtime_path);
            return 0;
        }
        fclose(runtime);
    }

    footer_status = read_footer(runtime_path, &archive_offset, &archive_size, error, error_size);
    if (footer_status < 0) return 0;
    if (footer_status == 1) runtime_size = archive_offset;

    entry_name = copy_string(path_basename(entry_path));
    if (!entry_name || !add_entry(&entries, entry_name, entry_path, error, error_size)) goto done;
    for (uint32_t index = 0; index < include_count; ++index)
        if (!collect_input(include_paths[index], &entries, error, error_size)) goto done;

    temporary_path = append_string(output_path, ".tmp");
    archive_path = append_string(output_path, ".archive.tmp");
    if (!temporary_path || !archive_path) {
        set_error(error, error_size, "Failed to allocate temporary bundle path");
        goto done;
    }
    remove(temporary_path);
    remove(archive_path);

    memset(&writer, 0, sizeof(writer));
    if (!mz_zip_writer_init_file(&writer, archive_path, 0)) {
        set_error(error, error_size, "Failed to create bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
        goto done;
    }
    writer_open = 1;
    for (size_t index = 0; index < entries.length; ++index) {
        if (!mz_zip_writer_add_file(&writer, entries.values[index].archive_name, entries.values[index].source_path, NULL, 0, MZ_NO_COMPRESSION)) {
            set_error(error, error_size, "Failed to add '%s' to bundle: %s", entries.values[index].archive_name, mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
            goto done;
        }
    }
    if (!mz_zip_writer_add_mem(&writer, LT_BUNDLE_ENTRY_PATH, entry_name, strlen(entry_name), MZ_NO_COMPRESSION) ||
        !mz_zip_writer_finalize_archive(&writer)) {
        set_error(error, error_size, "Failed to finalize bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
        goto done;
    }
    if (!mz_zip_writer_end(&writer)) {
        set_error(error, error_size, "Failed to close bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
        writer_open = 0;
        goto done;
    }
    writer_open = 0;

    if (!copy_prefix(runtime_path, temporary_path, runtime_size, error, error_size) ||
        !append_file_contents(archive_path, temporary_path, error, error_size)) goto done;
    {
        FILE* file = fopen(archive_path, "rb");
        if (!file || !file_size(file, &archive_size)) {
            if (file) fclose(file);
            set_error(error, error_size, "Failed to measure bundle archive");
            goto done;
        }
        fclose(file);
        archive_offset = runtime_size;
    }
    if (!append_footer(temporary_path, archive_offset, archive_size, error, error_size)) goto done;
    remove(output_path);
    if (rename(temporary_path, output_path) != 0) {
        set_error(error, error_size, "Failed to move completed bundle to '%s'", output_path);
        goto done;
    }
#ifndef _WIN32
    chmod(output_path, 0755);
#endif
    result = 1;

done:
    if (writer_open) mz_zip_writer_end(&writer);
    if (!result && temporary_path) remove(temporary_path);
    if (archive_path) remove(archive_path);
    free(archive_path);
    free(temporary_path);
    free(entry_name);
    free_entry_list(&entries);
    return result;
}
