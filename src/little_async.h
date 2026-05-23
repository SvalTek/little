#pragma once

#include "little.h"

void ltasync_open_all(lt_VM* vm);
void ltasync_open_promise(lt_VM* vm);
void ltasync_open_timer(lt_VM* vm);
void ltasync_open_thread(lt_VM* vm);
