#include "little.h"
#include "little_buffer_internal.h"
#include "little_frontend_internal.h"
#include "little_runtime_internal.h"
#include "little_value_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void lt_push_diagnostic_internal(lt_VM* vm, lt_Span span, uint16_t line, uint16_t col, uint8_t severity, const char* message)
{
	if (!vm->collect_diagnostics) return;

	lt_Diagnostic diagnostic;
	diagnostic.span = span;
	diagnostic.line = line;
	diagnostic.col = col;
	diagnostic.severity = severity;
	diagnostic.code = 0;

	size_t length = strlen(message);
	diagnostic.message = vm->alloc(length + 1);
	memcpy(diagnostic.message, message, length + 1);

	lt_buffer_push(vm, &vm->diagnostics, &diagnostic);
}

void lt_syntax_error_internal(lt_VM* vm, const char* module, lt_Span span, uint16_t line, uint16_t col, const char* message)
{
	vm->error_line = line;
	vm->error_col = col;
	lt_push_diagnostic_internal(vm, span, line, col, LT_DIAG_ERROR, message);

	char sprint_buf[128];
	sprintf_s(sprint_buf, 128, "%s|%d:%d: %s", module, line, col, message);
	lt_error(vm, sprint_buf);
}

void lt_tokenize_error_internal(lt_VM* vm, const char* module, uint16_t line, uint16_t col, lt_Span span, const char* message)
{
	lt_syntax_error_internal(vm, module, span, line, col, message);
}

/* A token that starts a new statement even though newlines are whitespace. */
static uint8_t _lt_starts_statement(lt_TokenType type)
{
	switch (type)
	{
	case LT_TOKEN_IF:
	case LT_TOKEN_FOR:
	case LT_TOKEN_WHILE:
	case LT_TOKEN_WITH:
	case LT_TOKEN_VAR:
	case LT_TOKEN_GLOBAL:
	case LT_TOKEN_CLASS:
	case LT_TOKEN_FN:
	case LT_TOKEN_ASYNC:
	case LT_TOKEN_RETURN:
	case LT_TOKEN_BREAK:
	case LT_TOKEN_IMPORT:
		return 1;
	default:
		return 0;
	}
}

void lt_parse_error_internal(lt_VM* vm, const char* module, lt_Token* t, const char* message)
{
	vm->error_at_token = 1;
	lt_syntax_error_internal(vm, module, t->span, t->line, t->col, message);
}

static lt_DebugInfo* _lt_get_debuginfo(lt_Object* obj)
{
	switch (obj->type)
	{
	case LT_OBJECT_CHUNK: return obj->chunk.debug;
	case LT_OBJECT_FN: return obj->fn.debug;
	case LT_OBJECT_CLOSURE: {
		lt_Value function_value = obj->closure.function;
		if (!LT_IS_OBJECT(function_value)) return 0;
		lt_Object* function = LT_GET_OBJECT(function_value);
		if (function->type == LT_OBJECT_FN) return function->fn.debug;
		if (function->type == LT_OBJECT_CLOSURE) return _lt_get_debuginfo(function);
		return 0;
	}
	}

	return 0;
}

static lt_DebugLoc _lt_get_location(lt_DebugInfo* info, uint32_t pc)
{
	if (info && pc < info->locations.length)
	{
		return *(lt_DebugLoc*)lt_buffer_at(&info->locations, pc);
	}

	return (lt_DebugLoc){ 0, 0 };
}

/* Native class members are stored as a closure wrapping a native function, so a
   frame's callee is not always the native object itself. */
lt_Object* lt_native_callee_internal(lt_Object* callee)
{
	if (!callee) return 0;
	if (callee->type == LT_OBJECT_NATIVEFN || callee->type == LT_OBJECT_BOUND_NATIVE) return callee;
	if (callee->type == LT_OBJECT_CLOSURE && LT_IS_OBJECT(callee->closure.function))
		return lt_native_callee_internal(LT_GET_OBJECT(callee->closure.function));
	return 0;
}

void lt_runtime_error(lt_VM* vm, const char* message)
{
	char sprint_buf[1024];

	/* The innermost frame is frequently a native function, which carries no
	   source information. Report the nearest frame that has debug info, so the
	   message points at the script location that triggered the error. */
	lt_Frame* reported = 0;
	lt_DebugInfo* reported_info = 0;
	for (int32_t i = (int32_t)vm->depth - 1; i >= 0; --i)
	{
		lt_DebugInfo* info = _lt_get_debuginfo(vm->callstack[i].callee);
		if (info)
		{
			reported = &vm->callstack[i];
			reported_info = info;
			break;
		}
	}

	const char* name = "<unknown>";
	if (reported_info) name = reported_info->module_name;
	uint32_t pc = reported && reported->pc > 0 ? reported->pc - 1 : 0;
	lt_DebugLoc loc = _lt_get_location(reported_info, pc);

	int written = snprintf(sprint_buf, 1024, "%s|%d:%d: %s\ntraceback:", name, loc.line, loc.col, message);
	uint32_t len = written > 0 ? (uint32_t)written : 0;
	if (len >= 1024) len = 1023;
	for (int32_t i = (int32_t)vm->depth - 1; i >= 0; --i)
	{
		lt_Frame* frame = &vm->callstack[i];
		lt_DebugInfo* info = _lt_get_debuginfo(frame->callee);
		lt_Object* native = lt_native_callee_internal(frame->callee);
		if (native)
		{
			const char* native_name = 0;
			if (LT_IS_STRING(native->debug_name)) native_name = lt_get_string(vm, native->debug_name);
			if (native_name) written = snprintf(sprint_buf + len, 1024 - len, "\n(native|%s)", native_name);
			else written = snprintf(sprint_buf + len, 1024 - len, "\n(native)");
		}
		else if (!info)
		{
			/* A script frame without debug info (debug generation disabled). */
			written = snprintf(sprint_buf + len, 1024 - len, "\n(<unknown>|0:0)");
		}
		else
		{
			/* Each frame reports its own call site, not the start of its code. */
			uint32_t frame_pc = frame->pc > 0 ? frame->pc - 1 : 0;
			lt_DebugLoc frame_loc = _lt_get_location(info, frame_pc);
			written = snprintf(sprint_buf + len, 1024 - len, "\n(%s|%d:%d)", info->module_name, frame_loc.line, frame_loc.col);
		}
		if (written > 0) len += (uint32_t)written;
		if (len >= 1024) { len = 1023; break; }
	}

	lt_error(vm, sprint_buf);
}


void lt_diagnostics_clear(lt_VM* vm)
{
	for (uint32_t i = 0; i < vm->diagnostics.length; i++)
		vm->free(((lt_Diagnostic*)lt_buffer_at(&vm->diagnostics, i))->message);
	vm->diagnostics.length = 0;
}

uint32_t lt_diagnostic_count(lt_VM* vm)
{
	return vm->diagnostics.length;
}

const lt_Diagnostic* lt_diagnostic_get(lt_VM* vm, uint32_t index)
{
	if (index >= vm->diagnostics.length) return 0;
	return (const lt_Diagnostic*)lt_buffer_at(&vm->diagnostics, index);
}

void lt_error(lt_VM* vm, const char* msg)
{
	if (vm->trap_errors)
	{
		if (vm->error_trap) vm->free(vm->error_trap);
		uint32_t len = (uint32_t)strlen(msg);
		vm->error_trap = vm->alloc(len + 1);
		memcpy(vm->error_trap, msg, len + 1);
	}
	else if (vm->error) vm->error(vm, msg);
	if (vm->error_buf) longjmp(*(jmp_buf*)vm->error_buf, 1);
	abort();
}
