#include "little_bundle_internal.h"
#include "little_bundle_build.h"

#include "little_common.h"
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

typedef struct {
    char* archive_name;
    char* source_path;
} BundleEntry;

typedef struct {
    BundleEntry* values;
    size_t length;
    size_t capacity;
} BundleEntryList;

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
    return lt_bundle_copy_string(buffer);
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
        lt_bundle_set_error(error, error_size, "Failed to resolve included file '%s'", source_path);
        return 0;
    }
    if (strncmp(archive_name, "__little__/", 11) == 0 || strcmp(archive_name, "__little__") == 0) {
        free(canonical);
        lt_bundle_set_error(error, error_size, "Archive path '%s' is reserved", archive_name);
        return 0;
    }

    for (size_t index = 0; index < list->length; ++index) {
        if (strcmp(list->values[index].archive_name, archive_name) != 0) continue;
        if (strcmp(list->values[index].source_path, canonical) == 0) {
            free(canonical);
            return 1;
        }
        free(canonical);
        lt_bundle_set_error(error, error_size, "Bundle path collision: %s", archive_name);
        return 0;
    }

    if (list->length == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        BundleEntry* values = realloc(list->values, capacity * sizeof(*values));
        if (!values) {
            free(canonical);
            lt_bundle_set_error(error, error_size, "Failed to allocate bundle file list");
            return 0;
        }
        list->values = values;
        list->capacity = capacity;
    }
    list->values[list->length].archive_name = lt_bundle_copy_string(archive_name);
    list->values[list->length].source_path = canonical;
    if (!list->values[list->length].archive_name) {
        free(canonical);
        lt_bundle_set_error(error, error_size, "Failed to allocate bundle archive path");
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
    if (kind == -1) lt_bundle_set_error(error, error_size, "Symlinks and reparse points are not allowed in bundles: %s", path);
    else lt_bundle_set_error(error, error_size, "Included path does not exist: %s", path);
    return 0;
}

static int collect_directory(const char* directory, const char* archive_prefix, BundleEntryList* list, char* error, size_t error_size)
{
#ifdef _WIN32
    char* pattern = lt_bundle_join_host_path(directory, "*");
    WIN32_FIND_DATAA data;
    HANDLE handle;
    if (!pattern) {
        lt_bundle_set_error(error, error_size, "Failed to allocate directory path");
        return 0;
    }
    handle = FindFirstFileA(pattern, &data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        lt_bundle_set_error(error, error_size, "Failed to read directory '%s'", directory);
        return 0;
    }
    do {
        char* child_path;
        char* child_archive;
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        child_path = lt_bundle_join_host_path(directory, data.cFileName);
        child_archive = lt_bundle_join_archive_path(archive_prefix, data.cFileName);
        if (!child_path || !child_archive) {
            free(child_path);
            free(child_archive);
            FindClose(handle);
            lt_bundle_set_error(error, error_size, "Failed to allocate bundle path");
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
        lt_bundle_set_error(error, error_size, "Failed to read directory '%s'", directory);
        return 0;
    }
    FindClose(handle);
    return 1;
#else
    DIR* handle = opendir(directory);
    struct dirent* data;
    if (!handle) {
        lt_bundle_set_error(error, error_size, "Failed to read directory '%s'", directory);
        return 0;
    }
    while ((data = readdir(handle)) != NULL) {
        char* child_path;
        char* child_archive;
        if (strcmp(data->d_name, ".") == 0 || strcmp(data->d_name, "..") == 0) continue;
        child_path = lt_bundle_join_host_path(directory, data->d_name);
        child_archive = lt_bundle_join_archive_path(archive_prefix, data->d_name);
        if (!child_path || !child_archive) {
            free(child_path);
            free(child_archive);
            closedir(handle);
            lt_bundle_set_error(error, error_size, "Failed to allocate bundle path");
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
        lt_bundle_set_error(error, error_size, "Failed to close directory '%s'", directory);
        return 0;
    }
    return 1;
#endif
}

static int collect_input(const char* path, BundleEntryList* list, char* error, size_t error_size)
{
    int kind = path_kind(path);
    if (kind == 2) return collect_directory(path, "", list, error, error_size);
    if (kind == 1) return add_entry(list, lt_bundle_path_basename(path), path, error, error_size);
    if (kind == -1) lt_bundle_set_error(error, error_size, "Symlinks and reparse points are not allowed in bundles: %s", path);
    else lt_bundle_set_error(error, error_size, "Included path does not exist: %s", path);
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
        lt_bundle_set_error(error, error_size, "Failed to open runtime while creating bundle");
        return 0;
    }
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (fread(buffer, 1, chunk, source) != chunk || fwrite(buffer, 1, chunk, destination) != chunk) {
            fclose(source);
            fclose(destination);
            lt_bundle_set_error(error, error_size, "Failed to copy Little runtime into bundle");
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
        lt_bundle_set_error(error, error_size, "Failed to append bundle archive");
        return 0;
    }
    while ((read_count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, read_count, destination) != read_count) {
            fclose(source);
            fclose(destination);
            lt_bundle_set_error(error, error_size, "Failed to append bundle archive");
            return 0;
        }
    }
    if (ferror(source)) {
        fclose(source);
        fclose(destination);
        lt_bundle_set_error(error, error_size, "Failed to read bundle archive");
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
        lt_bundle_set_error(error, error_size, "Failed to append bundle footer to '%s'", path);
        return 0;
    }
    memcpy(footer, LT_BUNDLE_FOOTER_MAGIC, 8);
    lt_bundle_write_u64(footer + 8, archive_offset);
    lt_bundle_write_u64(footer + 16, archive_size);
    lt_bundle_write_u32(footer + 24, LT_BUNDLE_VERSION);
    lt_bundle_write_u32(footer + 28, 0);
    if (fwrite(footer, 1, sizeof(footer), file) != sizeof(footer)) {
        fclose(file);
        lt_bundle_set_error(error, error_size, "Failed to write bundle footer to '%s'", path);
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
        lt_bundle_set_error(error, error_size, "Runtime executable path is required");
        return 0;
    }
    if (!output_path) {
        lt_bundle_set_error(error, error_size, "Bundle output path is required");
        return 0;
    }
    if (!entry_path) {
        lt_bundle_set_error(error, error_size, "Bundle entry is required");
        return 0;
    }
    if (paths_equal(runtime_path, output_path)) {
        lt_bundle_set_error(error, error_size, "Bundle output must differ from the runtime executable");
        return 0;
    }
    if (path_kind(entry_path) != 1) {
        lt_bundle_set_error(error, error_size, "Bundle entry must be a regular file: %s", entry_path);
        return 0;
    }
    {
        FILE* runtime = fopen(runtime_path, "rb");
        if (!runtime || !lt_bundle_file_size(runtime, &runtime_size)) {
            if (runtime) fclose(runtime);
            lt_bundle_set_error(error, error_size, "Failed to read runtime executable '%s'", runtime_path);
            return 0;
        }
        fclose(runtime);
    }

    footer_status = lt_bundle_read_footer(runtime_path, &archive_offset, &archive_size, error, error_size);
    if (footer_status < 0) return 0;
    if (footer_status == 1) runtime_size = archive_offset;

    entry_name = lt_bundle_copy_string(lt_bundle_path_basename(entry_path));
    if (!entry_name || !add_entry(&entries, entry_name, entry_path, error, error_size)) goto done;
    for (uint32_t index = 0; index < include_count; ++index)
        if (!collect_input(include_paths[index], &entries, error, error_size)) goto done;

    temporary_path = lt_bundle_append_string(output_path, ".tmp");
    archive_path = lt_bundle_append_string(output_path, ".archive.tmp");
    if (!temporary_path || !archive_path) {
        lt_bundle_set_error(error, error_size, "Failed to allocate temporary bundle path");
        goto done;
    }
    remove(temporary_path);
    remove(archive_path);

    memset(&writer, 0, sizeof(writer));
    if (!mz_zip_writer_init_file(&writer, archive_path, 0)) {
        lt_bundle_set_error(error, error_size, "Failed to create bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
        goto done;
    }
    writer_open = 1;
    for (size_t index = 0; index < entries.length; ++index) {
        if (!mz_zip_writer_add_file(&writer, entries.values[index].archive_name, entries.values[index].source_path, NULL, 0, MZ_NO_COMPRESSION)) {
            lt_bundle_set_error(error, error_size, "Failed to add '%s' to bundle: %s", entries.values[index].archive_name, mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
            goto done;
        }
    }
    if (!mz_zip_writer_add_mem(&writer, LT_BUNDLE_ENTRY_PATH, entry_name, strlen(entry_name), MZ_NO_COMPRESSION) ||
        !mz_zip_writer_finalize_archive(&writer)) {
        lt_bundle_set_error(error, error_size, "Failed to finalize bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
        goto done;
    }
    if (!mz_zip_writer_end(&writer)) {
        lt_bundle_set_error(error, error_size, "Failed to close bundle archive: %s", mz_zip_get_error_string(mz_zip_get_last_error(&writer)));
        writer_open = 0;
        goto done;
    }
    writer_open = 0;

    if (!copy_prefix(runtime_path, temporary_path, runtime_size, error, error_size) ||
        !append_file_contents(archive_path, temporary_path, error, error_size)) goto done;
    {
        FILE* file = fopen(archive_path, "rb");
        if (!file || !lt_bundle_file_size(file, &archive_size)) {
            if (file) fclose(file);
            lt_bundle_set_error(error, error_size, "Failed to measure bundle archive");
            goto done;
        }
        fclose(file);
        archive_offset = runtime_size;
    }
    if (!append_footer(temporary_path, archive_offset, archive_size, error, error_size)) goto done;
    remove(output_path);
    if (rename(temporary_path, output_path) != 0) {
        lt_bundle_set_error(error, error_size, "Failed to move completed bundle to '%s'", output_path);
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
