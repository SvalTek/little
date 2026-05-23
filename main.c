#include <stdio.h>
#include <stdlib.h>

#include "src/little.h"
#include "src/little_std.h"
#include "src/little_async.h"

void error(lt_VM* vm, const char* msg)
{
    printf("LT ERROR: %s\n", msg);
}

int main(int argc, char** argv)
{
    // Load program
    if (argc != 2)
    {
        printf("Usage: little FILENAME\n");
        return 0;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp)
    {
        printf("ERROR: Failed to open '%s'\n", argv[1]);
        return 0;
    }
    static char text[1 << 20];
    fread(text, 1, sizeof(text), fp);
    fclose(fp);

    // Init VM and run program
    lt_VM* vm = lt_open(malloc, free, error);
    if (!vm)
    {
        printf("ERROR: Failed to initialize VM\n");
        return 1;
    }
    ltstd_open_all(vm);
    ltasync_open_all(vm);

    uint32_t nreturn = lt_dostring(vm, text, "module");
    lt_runloop(vm);

    while (nreturn-- > 0)
    {
        char* returned = ltstd_tostring(vm, lt_pop(vm));
        printf("Returned: %s\n", returned);
        free(returned);
    }

    lt_destroy(vm);

    return 0;
}
