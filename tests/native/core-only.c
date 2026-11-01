#include "little.h"

#include <stdlib.h>

int main(void)
{
	lt_VM* vm = lt_open(malloc, free, NULL);
	if (!vm) return 1;
	if (lt_dostring(vm, "return 6 * 7", "<core-only>") != 1) {
		lt_destroy(vm);
		return 2;
	}
	lt_Value result = lt_pop(vm);
	int ok = LT_IS_NUMBER(result) && lt_get_number(result) == 42;
	lt_destroy(vm);
	return ok ? 0 : 3;
}
