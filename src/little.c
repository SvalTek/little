#include "little.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"
#include "little_async_internal.h"
#include "little_opcode.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
lt_VM* lt_open(lt_AllocFn alloc, lt_FreeFn free, lt_ErrorFn error)
{
	lt_VM* vm = alloc(sizeof(lt_VM));
	if (!vm) return 0;
	memset(vm, 0, sizeof(lt_VM));

	vm->alloc = alloc;
	vm->free = free;
	vm->error = error;

	vm->heap = lt_buffer_new(sizeof(lt_Object*));
	vm->keepalive = lt_buffer_new(sizeof(lt_Object*));
	vm->temporary_roots = lt_buffer_new(sizeof(lt_Object*));
	vm->native_roots = lt_buffer_new(sizeof(lt_Value));
	vm->native_libraries = lt_buffer_new(sizeof(lt_NativeLibrary));
	vm->module_loaders = lt_buffer_new(sizeof(lt_ModuleLoader));
	vm->diagnostics = lt_buffer_new(sizeof(lt_Diagnostic));
	ltasync_init_state(vm);

	vm->generate_debug = 1;

	vm->global = LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_TABLE));
	lt_nocollect(vm, LT_GET_OBJECT(vm->global));

	return vm;
}

void lt_destroy(lt_VM* vm)
{
	ltasync_destroy_state(vm);
	/* Drop every VM root and collect while plugin callbacks are still loaded. */
	vm->top = 0;
	vm->keepalive.length = 0;
	vm->temporary_roots.length = 0;
	vm->native_roots.length = 0;
	lt_collect(vm);
	for (uint32_t i = 0; i < vm->native_libraries.length; ++i)
	{
		lt_NativeLibrary* library = lt_buffer_at(&vm->native_libraries, i);
		if (library->handle && library->close) library->close(library->handle);
	}
	lt_buffer_destroy(vm, &vm->native_libraries);
	lt_buffer_destroy(vm, &vm->module_loaders);
	lt_buffer_destroy(vm, &vm->keepalive);
	lt_buffer_destroy(vm, &vm->temporary_roots);
	lt_buffer_destroy(vm, &vm->native_roots);
	lt_diagnostics_clear(vm);
	lt_buffer_destroy(vm, &vm->diagnostics);
	if (vm->error_trap) vm->free(vm->error_trap);
	vm->free(vm);
}


void lt_set_worker_vm_init(lt_VM* vm, lt_VMInitFn init)
{
	vm->worker_vm_init = init;
}

void lt_add_module_loader(lt_VM* vm, lt_ModuleLoaderFn loader, void* userdata)
{
	if (!loader) return;
	lt_ModuleLoader entry = { loader, userdata };
	lt_buffer_push(vm, &vm->module_loaders, &entry);
}


void lt_push(lt_VM* vm, lt_Value val)
{
	if (vm->top >= LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");
	vm->stack[vm->top++] = val;
}

lt_Value lt_pop(lt_VM* vm)
{
	if (vm->top == 0) lt_runtime_error(vm, "VM stack underflow!");
	return vm->stack[--vm->top];
}

lt_Value lt_at(lt_VM* vm, uint32_t idx)
{
	return vm->stack[vm->current->start + idx];
}

static lt_Value _lt_make_cell(lt_VM* vm, lt_Value value)
{
	lt_Object* cell = lt_allocate(vm, LT_OBJECT_CELL);
	cell->cell = value;
	return LT_VALUE_OBJECT(cell);
}

static lt_Value _lt_cell_get(lt_Value cell)
{
	if (!LT_IS_CELL(cell)) return cell;
	return LT_GET_OBJECT(cell)->cell;
}

static void _lt_cell_set(lt_VM* vm, lt_Value* slot, lt_Value value)
{
	if (!LT_IS_CELL(*slot))
	{
		*slot = _lt_make_cell(vm, *slot);
	}
	LT_GET_OBJECT(*slot)->cell = value;
}

static lt_Value _lt_capture_local(lt_VM* vm, uint16_t idx)
{
	lt_Value* slot = &vm->stack[vm->current->start + idx];
	if (!LT_IS_CELL(*slot))
	{
		*slot = _lt_make_cell(vm, *slot);
	}
	return *slot;
}

void lt_close(lt_VM* vm, uint8_t count)
{
	lt_Object* closure = lt_allocate(vm, LT_OBJECT_CLOSURE);
	closure->closure.captures = lt_buffer_new(sizeof(lt_Value));
	for (int i = 0; i < count; i++)
	{
		lt_Value v = lt_pop(vm);
		if (!LT_IS_CELL(v)) v = _lt_make_cell(vm, v);
		lt_buffer_push(vm, &closure->closure.captures, &v);
	}
	closure->closure.function = lt_pop(vm);
	lt_push(vm, LT_VALUE_OBJECT(closure));
}

lt_Value lt_getupval(lt_VM* vm, uint8_t idx)
{
	if (vm->current->upvals == 0) return LT_VALUE_NULL;
	return _lt_cell_get(*(lt_Value*)lt_buffer_at(vm->current->upvals, idx));
}

void lt_setupval(lt_VM* vm, uint8_t idx, lt_Value val)
{
	if (vm->current->upvals == 0) return;
	lt_Value* slot = lt_buffer_at(vm->current->upvals, idx);
	_lt_cell_set(vm, slot, val);
}

uint16_t lt_exec_internal(lt_VM* vm, lt_Value callable, uint8_t argc);


uint16_t lt_exec(lt_VM* vm, lt_Value callable, uint8_t argc)
{
	uint16_t base = vm->top - argc;
	uint16_t saved_depth = vm->depth;
	uint32_t root_mark = vm->native_roots.length;
	uint32_t temporary_root_mark = vm->temporary_roots.length;
	uint32_t saved_root_floor = vm->native_root_floor;
	lt_Frame* saved_current = vm->current;
	void* saved_error_buf = vm->error_buf;
	jmp_buf error_buf;
	vm->error_buf = &error_buf;

	if (!setjmp(error_buf))
	{
		uint16_t nret = lt_exec_internal(vm, callable, argc);
		vm->error_buf = saved_error_buf;
		return nret;
	}
	else
	{
		/* Unwind the failed execution back to our own frame so reentrant
		   callers (e.g. natives that re-enter the VM) keep their frame,
		   argument stack and current pointer intact. */
		vm->top = base;
		vm->depth = saved_depth;
		vm->current = saved_current;
		vm->native_roots.length = root_mark;
		vm->temporary_roots.length = temporary_root_mark;
		vm->native_root_floor = saved_root_floor;
		vm->error_buf = saved_error_buf;
		return 0;
	}
}

static uint8_t _lt_call_native(lt_VM* vm, lt_NativeFn fn, uint8_t argc)
{
	uint32_t root_mark = vm->native_roots.length;
	uint32_t saved_root_floor = vm->native_root_floor;
	vm->native_root_floor = root_mark;
	uint8_t n_return = fn(vm, argc);
	vm->native_roots.length = root_mark;
	vm->native_root_floor = saved_root_floor;
	return n_return;
}


uint16_t lt_exec_internal(lt_VM* vm, lt_Value callable, uint8_t argc)
{
	if (!LT_IS_OBJECT(callable))
	{
		lt_runtime_error(vm, "Value is not callable!");
		return 0;
	}
	if (LT_IS_CLASS(callable))
	{
		lt_push(vm, lt_class_call_internal(vm, callable, argc));
		return 1;
	}

	lt_Object* callee = LT_GET_OBJECT(callable);

	if (vm->depth >= LT_CALLSTACK_SIZE)
	{
		lt_runtime_error(vm, "Call stack overflow!");
	}

	lt_Frame* frame = &vm->callstack[vm->depth++];
	memset(frame, 0, sizeof(lt_Frame));
	vm->current = frame;

	uint16_t start = vm->top;

	frame->callee = callee;
	frame->start = vm->top - argc;

	switch (callee->type)
	{
	case LT_OBJECT_CHUNK: {
		frame->code = &callee->chunk.code;
		frame->constants = &callee->chunk.constants;
	} break;
	case LT_OBJECT_FN: {
		frame->code = &callee->fn.code;
		frame->constants = &callee->fn.constants;
		frame->class_context = callee->fn.owner_class;
	} break;
	case LT_OBJECT_CLOSURE: {
		lt_Object* fn = LT_GET_OBJECT(callee->closure.function);
		frame->upvals = &callee->closure.captures;
		frame->class_context = callee->closure.owner_class;
		if (fn->type == LT_OBJECT_FN)
		{
			frame->code = &fn->fn.code;
			frame->constants = &fn->fn.constants;
			if (!frame->class_context) frame->class_context = fn->fn.owner_class;
		}
		else
		{
			uint8_t n_return = _lt_call_native(vm, fn->native, argc);

			--vm->depth;
			vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
			return n_return;
		}
	} break;
	case LT_OBJECT_NATIVEFN: {
		uint8_t n_return = _lt_call_native(vm, callee->native, argc);

		--vm->depth;
		vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
		return n_return;
	} break;
	case LT_OBJECT_BOUND_NATIVE: {
		uint8_t n_return = _lt_call_native(vm, callee->bound_native.native, argc);

		--vm->depth;
		vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
		return n_return;
	} break;
	default:
		lt_runtime_error(vm, "Value is not callable!");
		return 0;
	}

	lt_Op current = *(lt_Op*)lt_buffer_at(frame->code, frame->pc++);
#undef NEXT
#define NEXT { current = *(lt_Op*)lt_buffer_at(frame->code, frame->pc++); goto inst_loop; break; }

#define PUSH(x) lt_push(vm, (x))
#define POP() lt_pop(vm)

inst_loop:
	switch (current.op)
	{
	case LT_OP_NOP: NEXT;

	case LT_OP_PUSH: {
		for (int i = 0; i < current.arg; i++)
		{
			PUSH(LT_VALUE_NULL);
		}
	} NEXT;

	case LT_OP_POP: {
		if (current.arg > vm->top) lt_runtime_error(vm, "VM stack underflow!");
		vm->top -= current.arg;
	} NEXT;
	case LT_OP_DUP: PUSH(vm->stack[vm->top - 1]); NEXT;
	case LT_OP_PUSHC: PUSH(*(lt_Value*)lt_buffer_at(frame->constants, current.arg)); NEXT;
	case LT_OP_PUSHN: PUSH(LT_VALUE_NULL); NEXT;
	case LT_OP_PUSHT: PUSH(LT_VALUE_TRUE); NEXT;
	case LT_OP_PUSHF: PUSH(LT_VALUE_FALSE); NEXT;

	case LT_OP_MAKET: {
		lt_Value t = LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_TABLE));
		for (uint32_t i = 0; i < (uint32_t)current.arg; ++i)
		{
			lt_Value value = POP();
			lt_Value key = POP();
			lt_table_set(vm, t, key, value);
		}
		PUSH(t);
	} NEXT;

	case LT_OP_MAKEA: {
		lt_Value a = LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_ARRAY));
		for (uint32_t i = 0; i < (uint32_t)current.arg; ++i)
		{
			lt_Value value = POP();
			lt_array_push(vm, a, value);
		}
		PUSH(a);
	} NEXT;

	case LT_OP_MAKEC: {
		PUSH(lt_make_class_internal(vm, POP()));
	} NEXT;

	case LT_OP_SETSUPER: {
		lt_Value superclass = POP();
		lt_Value klass = POP();
		lt_class_set_superclass_internal(vm, klass, superclass);
	} NEXT;

	case LT_OP_SETT: {
		lt_Value value = POP();
		lt_Value key = POP();
		lt_Value t = POP();
		if (LT_IS_TABLE(t))
		{
			lt_table_set(vm, t, key, value);
		}
		else if (LT_IS_ARRAY(t))
		{
			lt_array_set(vm, t, (uint32_t)lt_get_number(key), value);
		}
		else if (LT_IS_INSTANCE(t))
		{
			lt_instance_set_internal(vm, t, key, value);
		}
		else {};
	} NEXT;

	case LT_OP_SETC: {
		lt_Value key = POP();
		lt_Value klass = POP();
		lt_Value value = POP();
		lt_class_set_member_internal(vm, klass, key, value, current.arg);
	} NEXT;

	case LT_OP_GETT: {
		lt_Value key = POP();
		lt_Value t = POP();

		if (LT_IS_TABLE(t))
		{
			PUSH(lt_table_get(vm, t, key));
		}
		else if (LT_IS_OBJECT(t) && LT_GET_OBJECT(t)->type == LT_OBJECT_PROMISE && LT_IS_STRING(key))
		{
			PUSH(ltasync_get_promise_method(vm, t, key));
		}
		else if (LT_IS_INSTANCE(t))
		{
			PUSH(lt_instance_get_internal(vm, t, key));
		}
		else if (LT_IS_ARRAY(t))
		{
			PUSH(lt_array_get(vm, t, (uint32_t)lt_get_number(key)));
		}
		else PUSH(LT_VALUE_NULL);
	} NEXT;

	case LT_OP_GETD: {
		lt_Value key = POP();
		lt_Value t = POP();

		if (LT_IS_TABLE(t))
		{
			PUSH(lt_table_get(vm, t, key));
		}
		else if (LT_IS_INSTANCE(t))
		{
			PUSH(lt_instance_get_internal(vm, t, key));
		}
		else if (LT_IS_ARRAY(t) && LT_IS_NUMBER(key))
		{
			uint32_t idx = (uint32_t)lt_get_number(key);
			PUSH(idx < lt_array_length(t) ? lt_array_get(vm, t, idx) : LT_VALUE_NULL);
		}
		else PUSH(LT_VALUE_NULL);
	} NEXT;

	case LT_OP_GETG: {
		lt_Value key = POP();
		PUSH(lt_table_get(vm, vm->global, key));
	} NEXT;

	case LT_OP_SETG: {
		lt_Value key = POP();
		lt_Value value = POP();
		lt_table_set(vm, vm->global, key, value);
	} NEXT;

#define PASTE(x) x

#define IMPL_ARITH(op){                                                       \
		lt_Value right = POP();		                                          \
		lt_Value left = POP();		                                          \
		if (!LT_IS_NUMBER(right) || !LT_IS_NUMBER(left))                       \
			lt_runtime_error(vm, "Expected arithmetic operands to be numbers!"); \
		PUSH(lt_make_number(lt_get_number(left) PASTE(op) lt_get_number(right)));  \
	} NEXT;

	case LT_OP_ADD: IMPL_ARITH(+)
	case LT_OP_SUB: IMPL_ARITH(-)
	case LT_OP_MUL: IMPL_ARITH(*)
	case LT_OP_DIV: IMPL_ARITH(/)

	case LT_OP_EQ: {
		lt_Value right = POP();
		lt_Value left = POP();
		PUSH(lt_equals(left, right) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
	} NEXT;

	case LT_OP_NEQ: {
		lt_Value right = POP();
		lt_Value left = POP();
		PUSH(lt_equals(left, right) ? LT_VALUE_FALSE : LT_VALUE_TRUE);
	} NEXT;

	case LT_OP_GT: {
		lt_Value right = POP();
		lt_Value left = POP();
		if (!LT_IS_NUMBER(right) || !LT_IS_NUMBER(left))
			lt_runtime_error(vm, "Expected comparison operands to be numbers!");
		PUSH(lt_get_number(left) > lt_get_number(right) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
	} NEXT;

	case LT_OP_GTE: {
		lt_Value right = POP();
		lt_Value left = POP();
		if (!LT_IS_NUMBER(right) || !LT_IS_NUMBER(left))
			lt_runtime_error(vm, "Expected comparison operands to be numbers!");
		PUSH(lt_get_number(left) >= lt_get_number(right) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
	} NEXT;

	case LT_OP_NEG: {
		lt_Value right = POP();
		if (!LT_IS_NUMBER(right))
			lt_runtime_error(vm, "Expected negation operand to be a number!");
		PUSH(lt_make_number(lt_get_number(right) * -1.0));
	} NEXT;

	case LT_OP_AND: {
		lt_Value right = POP();
		lt_Value left = POP();
		PUSH(LT_IS_TRUTHY(right) && LT_IS_TRUTHY(left) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
	} NEXT;

	case LT_OP_OR: {
		lt_Value right = POP();
		lt_Value left = POP();
		if (LT_IS_TRUTHY(left)) PUSH(left);
		else if (LT_IS_TRUTHY(right)) PUSH(right);
		else PUSH(LT_VALUE_FALSE);
	} NEXT;

	case LT_OP_NOT: {
		lt_Value right = POP();
		PUSH(LT_IS_TRUTHY(right) ? LT_VALUE_FALSE : LT_VALUE_TRUE);
	} NEXT;

	case LT_OP_TYPE: {
		lt_Value value = POP();
		const char* type = "unknown";
		if (LT_IS_NULL(value)) type = "null";
		else if (LT_IS_BOOL(value)) type = "boolean";
		else if (LT_IS_NUMBER(value)) type = "number";
		else if (LT_IS_STRING(value)) type = "string";
		else if (LT_IS_OBJECT(value)) {
			switch (LT_GET_OBJECT(value)->type) {
			case LT_OBJECT_CHUNK:
			case LT_OBJECT_FN:
			case LT_OBJECT_CLOSURE:
			case LT_OBJECT_NATIVEFN:
			case LT_OBJECT_BOUND_NATIVE: type = "function"; break;
			case LT_OBJECT_TABLE:
			case LT_OBJECT_SHARED_TABLE: type = "table"; break;
			case LT_OBJECT_ARRAY:
			case LT_OBJECT_SHARED_ARRAY: type = "array"; break;
			case LT_OBJECT_PROMISE: type = "promise"; break;
			case LT_OBJECT_CLASS: type = "class"; break;
			case LT_OBJECT_INSTANCE: type = "instance"; break;
			case LT_OBJECT_PTR: type = "pointer"; break;
			case LT_OBJECT_CELL: type = "cell"; break;
			}
		}
		PUSH(lt_make_string(vm, type));
	} NEXT;

	case LT_OP_TYPEOF: {
		lt_Value value = POP();
		if (LT_IS_INSTANCE(value))
			PUSH(LT_VALUE_OBJECT(LT_GET_OBJECT(value)->instance.klass));
		else if (LT_IS_CLASS(value))
			PUSH(value);
		else
			PUSH(LT_VALUE_NULL);
	} NEXT;

	case LT_OP_LOAD: PUSH(vm->stack[frame->start + current.arg]); NEXT;
	case LT_OP_STORE: vm->stack[frame->start + current.arg] = POP(); NEXT;
	case LT_OP_LOADCELL: PUSH(_lt_cell_get(vm->stack[frame->start + current.arg])); NEXT;
	case LT_OP_STORECELL: {
		lt_Value value = POP();
		_lt_cell_set(vm, &vm->stack[frame->start + current.arg], value);
	} NEXT;

	case LT_OP_LOADUP: {
		PUSH(_lt_cell_get(*(lt_Value*)lt_buffer_at(frame->upvals, current.arg)));
	} NEXT;

	case LT_OP_STOREUP: {
		lt_Value value = POP();
		lt_Value* slot = lt_buffer_at(frame->upvals, current.arg);
		_lt_cell_set(vm, slot, value);
	} NEXT;

	case LT_OP_LOADUPCELL: {
		PUSH(*(lt_Value*)lt_buffer_at(frame->upvals, current.arg));
	} NEXT;

	case LT_OP_CAPTURE: {
		PUSH(_lt_capture_local(vm, (uint16_t)current.arg));
	} NEXT;

	case LT_OP_CLOSE: {
		lt_Object* closure = lt_allocate(vm, LT_OBJECT_CLOSURE);
		closure->closure.captures = lt_buffer_new(sizeof(lt_Value));
		for (int i = 0; i < current.arg; i++)
		{
			lt_Value capture = POP();
			if (!LT_IS_CELL(capture)) capture = _lt_make_cell(vm, capture);
			lt_buffer_push(vm, &closure->closure.captures, &capture);
		}
		closure->closure.function = POP();
		PUSH(LT_VALUE_OBJECT(closure));
	} NEXT;

	case LT_OP_CALL: {
		lt_Value callee = POP();
		if (LT_IS_CLASS(callee))
		{
			lt_Value result = lt_class_call_internal(vm, callee, (uint8_t)current.arg);
			PUSH(result);
			vm->last_call_returns = 1;
		}
		else if (ltasync_is_async_callable(callee))
		{
			lt_Value result = ltasync_call(vm, callee, (uint8_t)current.arg);
			PUSH(result);
			vm->last_call_returns = 1;
		}
		else vm->last_call_returns = (uint8_t)lt_exec_internal(vm, callee, (uint8_t)current.arg);
	} NEXT;

	case LT_OP_CALLM: {
		uint8_t spread = vm->last_call_returns;
		lt_Value callee = POP();
		uint8_t argc = (uint8_t)(current.arg + spread);
		if (LT_IS_CLASS(callee))
		{
			lt_Value result = lt_class_call_internal(vm, callee, argc);
			PUSH(result);
			vm->last_call_returns = 1;
		}
		else if (ltasync_is_async_callable(callee))
		{
			lt_Value result = ltasync_call(vm, callee, argc);
			PUSH(result);
			vm->last_call_returns = 1;
		}
		else vm->last_call_returns = (uint8_t)lt_exec_internal(vm, callee, argc);
	} NEXT;

	case LT_OP_SUPERC: {
		if (!frame->class_context || !frame->class_context->class_def.superclass) lt_runtime_error(vm, "No superclass constructor to call!");
		lt_Object* superclass = frame->class_context->class_def.superclass;
		lt_Value constructor = lt_find_class_constructor_internal(superclass);
		if (LT_IS_NULL(constructor)) lt_runtime_error(vm, "Superclass has no constructor!");
		uint8_t argc = (uint8_t)current.arg;
		uint16_t base = vm->top - argc;
		for (uint8_t i = 0; i < argc; ++i)
			vm->stack[vm->top - i] = vm->stack[vm->top - i - 1];
		vm->stack[base] = vm->stack[frame->start];
		vm->top++;
		if (vm->top > LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");
		uint16_t nret = lt_exec_internal(vm, constructor, argc + 1);
		while (nret-- > 0) lt_pop(vm);
		PUSH(LT_VALUE_NULL);
		vm->last_call_returns = 1;
	} NEXT;

	case LT_OP_SUPERM: {
		lt_Value key = POP();
		if (!frame->class_context || !frame->class_context->class_def.superclass) lt_runtime_error(vm, "No superclass method to call!");
		lt_TablePair* method = 0;
		for (lt_Object* current_class = frame->class_context->class_def.superclass; current_class; current_class = current_class->class_def.superclass)
		{
			method = lt_table_index_raw_internal(vm, &current_class->class_def.public_methods, key, 0);
			if (method) break;
		}
		if (!method) lt_runtime_error(vm, "Superclass method not found!");
		uint8_t argc = (uint8_t)current.arg;
		uint16_t base = vm->top - argc;
		for (uint8_t i = 0; i < argc; ++i)
			vm->stack[vm->top - i] = vm->stack[vm->top - i - 1];
		vm->stack[base] = vm->stack[frame->start];
		vm->top++;
		if (vm->top > LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");
		vm->last_call_returns = (uint8_t)lt_exec_internal(vm, method->value, argc + 1);
	} NEXT;

	case LT_OP_FIXRET: {
		uint8_t nret = vm->last_call_returns;
		uint16_t start = vm->top - nret;
		if (nret > current.arg) vm->top = start + current.arg;
		else while (nret++ < current.arg) PUSH(LT_VALUE_NULL);
		vm->last_call_returns = (uint8_t)current.arg;
	} NEXT;

	case LT_OP_PACKRET: {
		uint8_t nret = vm->last_call_returns;
		if (nret == 0) PUSH(LT_VALUE_NULL);
		else if (nret > 1)
		{
			lt_Value a = LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_ARRAY));
			uint16_t start = vm->top - nret;
			for (uint32_t i = 0; i < nret; ++i)
				lt_array_push(vm, a, vm->stack[start + i]);
			vm->top = start;
			PUSH(a);
		}
		vm->last_call_returns = 1;
	} NEXT;

	case LT_OP_AWAIT: {
		if (!ltasync_is_async_callable(LT_VALUE_OBJECT(frame->callee)))
			lt_runtime_error(vm, "'await' is only valid inside async functions!");
		lt_Value result = ltasync_await(vm, POP());
		PUSH(result);
	} NEXT;

	case LT_OP_ITER: {
		PUSH(lt_make_iter_internal(vm, POP(), (uint8_t)current.arg));
	} NEXT;

	case LT_OP_JMP:
	case LT_OP_BREAK: /* `break` placeholder, resolved by its loop at compile time */
		frame->pc += current.arg;
		NEXT;
	case LT_OP_JMPC: {
		lt_Value cond = POP();
		if (!LT_IS_TRUTHY(cond)) frame->pc += current.arg;
	} NEXT;
	case LT_OP_JMPN: {
		lt_Value cond = POP();
		if (cond == LT_VALUE_NULL) frame->pc += current.arg;
	} NEXT;

	case LT_OP_RET: {
		if (current.arg)
		{
			lt_Value rval = POP();
			vm->top = frame->start;
			--vm->depth;
			vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
			PUSH(rval);
			return 1;
		}
		else
		{
			vm->top = frame->start;
			--vm->depth;
			vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
			return 0;
		}
	} NEXT;

	case LT_OP_RETM: {
		uint8_t nret = vm->last_call_returns;
		lt_Value values[LT_MAX_RETURNS];
		for (uint8_t i = 0; i < nret; ++i)
			values[i] = vm->stack[vm->top - nret + i];
		vm->top = frame->start;
		--vm->depth;
		vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
		for (uint8_t i = 0; i < nret; ++i) PUSH(values[i]);
		return nret;
	} NEXT;

	default: lt_runtime_error(vm, "VM encountered unknown opcode!");
	}

	return vm->top - start;
}

#define OP(op) { lt_Op op = { LT_OP_##op, 0 }; lt_buffer_push(vm, code_body, &op); if(debug) { lt_buffer_push(vm, debug, &node->loc); } }
#define OPARG(op, arg) { lt_Op op = { LT_OP_##op, arg }; lt_buffer_push(vm, code_body, &op); if(debug) { lt_buffer_push(vm, debug, &node->loc); } }


lt_Value lt_loadstring(lt_VM* vm, const char* source, const char* mod_name)
{
	lt_Tokenizer tok = lt_tokenize(vm, source, mod_name);
	if (!tok.is_valid)
	{
		lt_free_tokenizer(vm, &tok);
		return LT_VALUE_NULL;
	}

	lt_Parser p = lt_parse(vm, &tok);
	if (!p.is_valid)
	{
		lt_free_parser(vm, &p);
		lt_free_tokenizer(vm, &tok);
		return LT_VALUE_NULL;
	}

	void* saved_error_buf = vm->error_buf;
	jmp_buf error_buf;
	vm->error_buf = &error_buf;
	if (setjmp(error_buf))
	{
		lt_free_parser(vm, &p);
		lt_free_tokenizer(vm, &tok);
		vm->error_buf = saved_error_buf;
		return LT_VALUE_NULL;
	}

	lt_Value c = lt_compile(vm, &p);
	vm->error_buf = saved_error_buf;

	lt_free_parser(vm, &p);
	lt_free_tokenizer(vm, &tok);

	return c;
}

uint32_t lt_dostring(lt_VM* vm, const char* source, const char* mod_name)
{
	lt_Value callable = lt_loadstring(vm, source, mod_name);
	return callable == LT_VALUE_NULL ? 0 : lt_exec(vm, callable, 0);
}
