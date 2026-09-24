#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* prefix = "LITTLE-RUNTIME-PREFIX";

static int file_size(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long length = ftell(file);
    fclose(file);
    if (length < 0) return 0;
    *size = (size_t)length;
    return 1;
}

static int append_file(const char* source_path, const char* destination_path)
{
    FILE* source_file = fopen(source_path, "rb");
    FILE* destination_file = fopen(destination_path, "wb");
    unsigned char buffer[4096];
    size_t read_count;

    if (!source_file || !destination_file) {
        if (source_file) fclose(source_file);
        if (destination_file) fclose(destination_file);
        return 0;
    }

    if (fwrite(prefix, 1, strlen(prefix), destination_file) != strlen(prefix)) {
        fclose(source_file);
        fclose(destination_file);
        return 0;
    }

    while ((read_count = fread(buffer, 1, sizeof(buffer), source_file)) > 0) {
        if (fwrite(buffer, 1, read_count, destination_file) != read_count) {
            fclose(source_file);
            fclose(destination_file);
            return 0;
        }
    }

    if (ferror(source_file)) {
        fclose(source_file);
        fclose(destination_file);
        return 0;
    }

    fclose(source_file);
    fclose(destination_file);
    return 1;
}

static int check_entry(mz_zip_archive* archive, const char* name, const char* expected)
{
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(archive, name, &size, 0);
    int matches = data && size == strlen(expected) && memcmp(data, expected, size) == 0;
    mz_free(data);
    return matches;
}

int main(int argc, char** argv)
{
    const char* archive_path;
    const char* bundle_path;
    mz_zip_archive writer;
    mz_zip_archive reader;
    size_t archive_size;
    size_t prefix_size = strlen(prefix);
    int success = 0;

    if (argc != 3) return 1;
    archive_path = argv[1];
    bundle_path = argv[2];

    memset(&writer, 0, sizeof(writer));
    if (!mz_zip_writer_init_file(&writer, archive_path, 0)) return 2;
    if (!mz_zip_writer_add_mem(&writer, "main.little", "return 7", strlen("return 7"), MZ_NO_COMPRESSION) ||
        !mz_zip_writer_add_mem(&writer, "lib/helper.little", "return 8", strlen("return 8"), MZ_NO_COMPRESSION) ||
        !mz_zip_writer_finalize_archive(&writer)) {
        mz_zip_writer_end(&writer);
        return 3;
    }
    if (!mz_zip_writer_end(&writer)) return 4;

    if (!file_size(archive_path, &archive_size) || !append_file(archive_path, bundle_path)) return 5;

    memset(&reader, 0, sizeof(reader));
    if (!mz_zip_reader_init_file_v2(&reader, bundle_path, 0, prefix_size, archive_size)) return 6;
    success = check_entry(&reader, "main.little", "return 7") &&
        check_entry(&reader, "lib/helper.little", "return 8");
    mz_zip_reader_end(&reader);

    remove(archive_path);
    remove(bundle_path);
    return success ? 0 : 7;
}
