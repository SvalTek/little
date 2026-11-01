#pragma once

#include "little.h"

uint8_t lt_buffer_push(lt_VM* vm, lt_Buffer* buf, void* element);
void* lt_buffer_at(lt_Buffer* buf, uint32_t idx);
void* lt_buffer_last(lt_Buffer* buf);
void lt_buffer_cycle(lt_Buffer* buf, uint32_t idx);
void lt_buffer_pop(lt_Buffer* buf);
