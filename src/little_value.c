#include "little.h"
#include "little_buffer_internal.h"
#include "little_value_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Microsoft provides these bounds-checked functions; provide the small subset
 * Little uses on other C runtimes. */
#if !defined(_WIN32)
#include <stdarg.h>

int sprintf_s(char *restrict buffer, size_t bufsz, const char *restrict format, ...)
{
	va_list args;
    va_start(args, format);
	int r = vsnprintf(buffer, bufsz, format, args);
	va_end(args);
	return r;
}

int strncpy_s(char *restrict dest, size_t destsz, const char *restrict src, size_t count)
{
	if (!destsz) return 1;
	if (!src) {
		dest[0] = 0;
		return 1;
	}

	if (count >= destsz) count = destsz - 1;
	memcpy(dest, src, count);
	dest[count] = 0;
	return 0;
}
#endif

uint64_t static MurmurOAAT64(const char* key)
{
	uint64_t h = 525201411107845655ull;
	for (; *key; ++key) {
		h ^= *key;
		h *= 0x5bd1e9955bd1e995;
		h ^= h >> 47;
	}
	return h;
}

typedef union {
	double flt;
	uint64_t bits;
} _lt_conversion_union;


lt_Value lt_make_number(double n)
{
	_lt_conversion_union u;
	u.flt = n;
	return (lt_Value)u.bits;
}

double lt_get_number(lt_Value v)
{
	_lt_conversion_union u;
	u.bits = v;
	return (double)u.flt;
}

lt_Value lt_make_string(lt_VM* vm, const char* string)
{
	uint32_t len = (uint32_t)strlen(string);
	uint64_t hash = MurmurOAAT64(string);
	uint16_t bucket = hash % LT_DEDUP_TABLE_SIZE;

	lt_Buffer* buf = vm->strings + bucket;
	if (buf->element_size == 0) *buf = lt_buffer_new(sizeof(lt_StringDedupEntry));

	int32_t first_empty = -1;
	for (uint32_t i = 0; i < buf->length; i++)
	{
		lt_StringDedupEntry* entry = lt_buffer_at(buf, i);
		if (entry->hash == hash)
		{
			return entry->value;
		}
		else if (entry->hash == 0 && first_empty == -1) first_empty = i;
	}

	lt_StringDedupEntry new_entry;
	new_entry.hash = hash;
	new_entry.len = len;
	new_entry.refcount = 0;

	new_entry.string = vm->alloc(len + 1);
	memcpy(new_entry.string, string, len);
	new_entry.string[len] = 0;

	uint32_t index = 0;
	if (first_empty != -1)
	{
		index = first_empty;
		lt_StringDedupEntry* e = lt_buffer_at(buf, first_empty);
		new_entry.value = (LT_NAN_MASK | LT_TYPE_STRING) | (bucket << 24) | (index & 0xFFFFFF);
		memcpy(e, &new_entry, sizeof(lt_StringDedupEntry));
		return new_entry.value;
	}
	else
	{
		lt_buffer_push(vm, buf, &new_entry);
		lt_StringDedupEntry* e = lt_buffer_at(buf, buf->length - 1);
		index = buf->length - 1;
		e->value = (LT_NAN_MASK | LT_TYPE_STRING) | (bucket << 24) | (index & 0xFFFFFF);
		return e->value;
	}
}

const char* lt_get_string(lt_VM* vm, lt_Value value)
{
	uint16_t bucket = (uint16_t)((value & 0xFFFFFF000000) >> 24);
	uint32_t index = value & 0xFFFFFF;
	return ((lt_StringDedupEntry*)lt_buffer_at(vm->strings + bucket, index))->string;
}

void lt_reference_string_internal(lt_VM* vm, lt_Value value)
{
	uint16_t bucket = (uint16_t)((value & 0xFFFFFF000000) >> 24);
	uint32_t index = value & 0xFFFFFF;
	((lt_StringDedupEntry*)lt_buffer_at(vm->strings + bucket, index))->refcount++;
}

uint8_t faststrcmp(const char* a, uint64_t a_len, const char* b, uint64_t b_len)
{
	if (a_len != b_len) return 0;
	for (int i = 0; i < a_len; ++i)
	{
		if (*(a + i) != *(b + i)) return 0;
	}

	return 1;
}

uint8_t lt_equals(lt_Value a, lt_Value b)
{
	if (LT_IS_NUMBER(a) || LT_IS_NUMBER(b))
		return LT_IS_NUMBER(a) && LT_IS_NUMBER(b) && lt_get_number(a) == lt_get_number(b);
	if ((a & LT_TYPE_MASK) != (b & LT_TYPE_MASK)) return 0;
	switch (a & LT_TYPE_MASK)
	{
	case LT_TYPE_NULL:
	case LT_TYPE_BOOL:
	case LT_TYPE_STRING:
		return a == b;

	case LT_TYPE_OBJECT: {
		lt_Object* obja = LT_GET_OBJECT(a);
		lt_Object* objb = LT_GET_OBJECT(b);
		if (obja->type != objb->type) return 0;

		switch (obja->type)
		{
		case LT_OBJECT_CHUNK:
		case LT_OBJECT_CLOSURE:
		case LT_OBJECT_FN:
		case LT_OBJECT_TABLE:
		case LT_OBJECT_NATIVEFN:
		case LT_OBJECT_BOUND_NATIVE:
		case LT_OBJECT_PROMISE:
		case LT_OBJECT_CLASS:
		case LT_OBJECT_INSTANCE:
		case LT_OBJECT_CELL:
		case LT_OBJECT_PTR:
			return obja == objb;
		case LT_OBJECT_SHARED_TABLE:
		case LT_OBJECT_SHARED_ARRAY:
			return obja->shared == objb->shared;
		}
	} break;
	}

	return 0;
}

#define HASH_VALUE(x) (LT_IS_OBJECT(x) ? (((x) >> 2) % 16) : ((x) % 16))

lt_Value lt_make_native(lt_VM* vm, lt_NativeFn fn)
{
	lt_Object* obj = lt_allocate(vm, LT_OBJECT_NATIVEFN);
	obj->native = fn;
	return LT_VALUE_OBJECT(obj);
}

lt_Value lt_make_ptr(lt_VM* vm, void* ptr)
{
	lt_Object* obj = lt_allocate(vm, LT_OBJECT_PTR);
	obj->ptr = ptr;
	return LT_VALUE_OBJECT(obj);
}

void* lt_get_ptr(lt_Value ptr)
{
	lt_Object* obj = LT_GET_OBJECT(ptr);
	return obj->ptr;
}
