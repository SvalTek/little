#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include "little_host_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

static char* copy_host_string(const char* value)
{
    size_t length = strlen(value);
    char* copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

char* lt_executable_path(const char* argv0)
{
#ifdef _WIN32
    char buffer[32768];
    DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)sizeof(buffer));
    if (length > 0 && length < sizeof(buffer)) return copy_host_string(buffer);
#elif defined(__linux__)
    char buffer[32768];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0 && length < (ssize_t)sizeof(buffer))
    {
        buffer[length] = 0;
        return copy_host_string(buffer);
    }
#elif defined(__APPLE__)
    {
        char buffer[32768];
        uint32_t size = (uint32_t)sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0)
        {
            char* resolved = realpath(buffer, NULL);
            if (resolved) return resolved;
            return copy_host_string(buffer);
        }
    }
#endif
#ifndef _WIN32
    {
        char* resolved = realpath(argv0, NULL);
        if (resolved) return resolved;
    }
#endif
    return copy_host_string(argv0);
}
