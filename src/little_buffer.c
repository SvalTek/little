#include "little_internal.h"

#include <stdlib.h>
#include <string.h>

lt_Buffer lt_buffer_new(uint32_t element_size)
{
	lt_Buffer buf;
	buf.element_size = element_size;
	buf.capacity = 0;
	buf.length = 0;
	buf.data = 0;

	return buf;
}

void lt_buffer_destroy(lt_VM* vm, lt_Buffer* buf)
{
	if (buf->data != 0) vm->free(buf->data);
	buf->data = 0;
	buf->length = 0;
	buf->capacity = 0;
}

uint8_t lt_buffer_push(lt_VM* vm, lt_Buffer* buf, void* element)
{
	uint8_t has_allocated = 0;
	if (buf->length + 1 > buf->capacity)
	{
		has_allocated = 1;

		void* new_buffer = vm->alloc(buf->element_size * (buf->capacity + 16));

		if (buf->data != 0)
		{
			memcpy(new_buffer, buf->data, buf->element_size * buf->capacity);
			free(buf->data);
		}

		buf->data = new_buffer;
		buf->capacity += 16;
	}

	memcpy((uint8_t*)buf->data + buf->element_size * buf->length, element, buf->element_size);
	buf->length++;

	return has_allocated;
}

void* lt_buffer_at(lt_Buffer* buf, uint32_t idx)
{
	return (uint8_t*)buf->data + buf->element_size * idx;
}

void* lt_buffer_last(lt_Buffer* buf)
{
	return lt_buffer_at(buf, buf->length - 1);
}

void lt_buffer_cycle(lt_Buffer* buf, uint32_t idx)
{
	memcpy(lt_buffer_at(buf, idx), lt_buffer_last(buf), buf->element_size);
	buf->length--;
}

void lt_buffer_pop(lt_Buffer* buf)
{
	buf->length--;
}
