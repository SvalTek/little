#include "little.h"
#include "little_buffer_internal.h"
#include "little_frontend_internal.h"
#include "little_opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP(op) { lt_Op op = { LT_OP_##op, 0 }; lt_buffer_push(vm, code_body, &op); if(debug) { lt_buffer_push(vm, debug, &node->loc); } }
#define OPARG(op, arg) { lt_Op op = { LT_OP_##op, arg }; lt_buffer_push(vm, code_body, &op); if(debug) { lt_buffer_push(vm, debug, &node->loc); } }
static uint16_t _lt_push_constant(lt_VM* vm, lt_Buffer* constants, lt_Value constant)
{
	for (uint32_t i = 0; i < constants->length; i++)
	{
		if ((*(lt_Value*)lt_buffer_at(constants, i)) == constant) return i;
	}

	if (constants->length >= LT_MAX_CONSTANTS) lt_error(vm, "Too many constants!");
	lt_buffer_push(vm, constants, &constant);
	return constants->length - 1;
}

static void _lt_compile_body(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_Buffer* ast_body, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants);
static void _lt_compile_node_ex(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_AstNode* node, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants, uint8_t allow_multi);
static uint16_t _lt_push_constant(lt_VM* vm, lt_Buffer* constants, lt_Value constant);
static void _lt_compile_node(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_AstNode* node, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants)
{
	_lt_compile_node_ex(vm, p, name, debug, node, scope, code_body, constants, 0);
}

static lt_Value _lt_compile_function_value(lt_VM* vm, lt_Parser* p, const char* name, lt_AstNode* node, lt_Object* owner_class)
{
	lt_Object* fn = lt_allocate(vm, LT_OBJECT_FN);

	uint8_t narg = 0;
	lt_Token** arg = node->fn.args;
	while (*arg) { narg++; arg++; }

	fn->fn.arity = narg;
	fn->fn.is_async = node->fn.is_async;
	fn->fn.owner_class = owner_class;
	fn->fn.code = lt_buffer_new(sizeof(lt_Op));
	fn->fn.constants = lt_buffer_new(sizeof(lt_Value));
	if (vm->generate_debug)
	{
		fn->fn.debug = vm->alloc(sizeof(lt_DebugInfo));
		fn->fn.debug->locations = lt_buffer_new(sizeof(lt_DebugLoc));
		fn->fn.debug->module_name = name;
	}

	lt_Buffer* debug = vm->generate_debug ? &fn->fn.debug->locations : 0;
	lt_Op op = { LT_OP_PUSH, 0 };
	lt_buffer_push(vm, &fn->fn.code, &op);
	if (debug) lt_buffer_push(vm, debug, &node->loc);

	_lt_compile_body(vm, p, name, debug, &node->fn.body, node->fn.scope, &fn->fn.code, &fn->fn.constants);

	lt_DebugLoc ret_loc = { 0, 0 };
	if (debug && debug->length > 0) ret_loc = *(lt_DebugLoc*)lt_buffer_at(debug, debug->length - 1);
	lt_Op op2 = { LT_OP_RET, 0 };
	lt_buffer_push(vm, &fn->fn.code, &op2);
	if (debug) lt_buffer_push(vm, debug, &ret_loc);

	((lt_Op*)lt_buffer_at(&fn->fn.code, 0))->arg = node->fn.scope->frame_slots.length;
	return LT_VALUE_OBJECT(fn);
}

static void _lt_compile_function_capture(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_AstNode* node, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants, lt_Object* owner_class, uint8_t force_closure)
{
	lt_Value as_val = _lt_compile_function_value(vm, p, name, node, owner_class);
	OPARG(PUSHC, _lt_push_constant(vm, constants, as_val));

	if (force_closure || node->fn.scope->upvals.length > 0)
	{
		lt_Buffer* upvals = &node->fn.scope->upvals;
		for (int i = upvals->length - 1; i >= 0; i--)
		{
			uint32_t idx = lt_find_local_internal(vm, scope, (lt_Token*)lt_buffer_at(upvals, i));
			if ((idx & UPVAL_BIT) == UPVAL_BIT) OPARG(LOADUPCELL, idx & 0xFFFF)
			else OPARG(CAPTURE, idx & 0xFFFF);
		}

		OPARG(CLOSE, upvals->length);
	}
}

static int16_t _lt_encode_class_member(lt_ClassMember* member)
{
	return (int16_t)(member->type | (member->visibility == LT_VIS_PRIVATE ? 0x10 : 0) | (member->is_override ? 0x20 : 0));
}

static void _lt_compile_index(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_AstNode* node, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants)
{
	_lt_compile_node(vm, p, name, debug, node->index.source, scope, code_body, constants);
	_lt_compile_node(vm, p, name, debug, node->index.idx, scope, code_body, constants);
}

static void _lt_compile_push_token_value(lt_VM* vm, lt_Parser* p, lt_Token* token, lt_Buffer* debug, lt_Buffer* code_body, lt_Buffer* constants, lt_DebugLoc* loc)
{
	uint16_t constant = 0;
	if (token->type == LT_TOKEN_NUMBER_LITERAL)
	{
		lt_Literal* literal = lt_buffer_at(&p->tkn->literal_buffer, token->idx);
		constant = _lt_push_constant(vm, constants, LT_VALUE_NUMBER(literal->number));
	}
	else
	{
		lt_Identifier* ident = lt_buffer_at(&p->tkn->identifier_buffer, token->idx);
		constant = _lt_push_constant(vm, constants, lt_make_string(vm, ident->name));
	}

	lt_Op op = { LT_OP_PUSHC, constant };
	lt_buffer_push(vm, code_body, &op);
	if (debug) lt_buffer_push(vm, debug, loc);
}

static void _lt_compile_node_ex(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_AstNode* node, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants, uint8_t allow_multi)
{
	switch (node->type)
	{
	case LT_AST_NODE_LITERAL: {
		lt_Token* t = node->literal.token;
		switch (t->type)
		{
		case LT_TOKEN_NULL_LITERAL: OP(PUSHN); break;
		case LT_TOKEN_TRUE_LITERAL: OP(PUSHT); break;
		case LT_TOKEN_FALSE_LITERAL: OP(PUSHF); break;
		case LT_TOKEN_NUMBER_LITERAL:
		case LT_TOKEN_STRING_LITERAL: {
			lt_Value con;
			lt_Literal* l = lt_buffer_at(&p->tkn->literal_buffer, t->idx);
			if (l->type == LT_TOKEN_NUMBER_LITERAL) con = LT_VALUE_NUMBER(l->number);
			else
			{
				con = lt_make_string(vm, l->string);
			}

			uint16_t idx = _lt_push_constant(vm, constants, con);
			OPARG(PUSHC, idx);
		} break;
		case LT_TOKEN_IDENTIFIER: {
			lt_Identifier* i = lt_buffer_at(&p->tkn->identifier_buffer, t->idx);
			lt_Value val = lt_make_string(vm, i->name);
			OPARG(PUSHC, _lt_push_constant(vm, constants, val));
		} break;
		default: {
			if (lt_token_is_keyword_internal(t))
			{
				lt_Identifier* i = lt_buffer_at(&p->tkn->identifier_buffer, t->idx);
				lt_Value val = lt_make_string(vm, i->name);
				OPARG(PUSHC, _lt_push_constant(vm, constants, val));
			}
			else lt_error(vm, "Invalid token in literal expression!");
		} break;
		}
	} break;
	case LT_AST_NODE_BREAK: {
		OPARG(BREAK, 0);
	} break;
	case LT_AST_NODE_TABLE: {
		uint16_t size = node->table.keys.length;

		for (int i = 0; i < size; ++i)
		{
			lt_AstNode* key = *(lt_AstNode**)lt_buffer_at(&node->table.keys, i);
			lt_AstNode* value = *(lt_AstNode**)lt_buffer_at(&node->table.values, i);
			_lt_compile_node(vm, p, name, debug, key, scope, code_body, constants);
			_lt_compile_node(vm, p, name, debug, value, scope, code_body, constants);
		}

		OPARG(MAKET, size);
	} break;

	case LT_AST_NODE_ARRAY: {
		uint16_t size = node->table.keys.length;

		for (int i = size - 1; i >= 0; --i)
		{
			lt_AstNode* value = *(lt_AstNode**)lt_buffer_at(&node->array.values, i);
			_lt_compile_node(vm, p, name, debug, value, scope, code_body, constants);
		}

		OPARG(MAKEA, size);
	} break;

	case LT_AST_NODE_IDENTIFIER: {
		uint32_t idx = lt_find_local_internal(vm, scope, node->identifier.token);
		if (idx == NOT_FOUND) {
			lt_Identifier* i = lt_buffer_at(&p->tkn->identifier_buffer, node->identifier.token->idx);
			lt_Value val = lt_make_string(vm, i->name);
			OPARG(PUSHC, _lt_push_constant(vm, constants, val));
			OP(GETG);
		}
		else if ((idx & UPVAL_BIT) == UPVAL_BIT)
		{
			OPARG(LOADUP, idx & 0xFFFF);
		}
		else if (lt_is_captured_local_internal(scope, idx))
		{
			OPARG(LOADCELL, idx & 0xFFFF);
		}
		else
		{
			OPARG(LOAD, idx & 0xFFFF);
		}
	} break;

	case LT_AST_NODE_INDEX: {
		_lt_compile_index(vm, p, name, debug, node, scope, code_body, constants);
		OP(GETT);
	} break;

	case LT_AST_NODE_SUPER: {
		OP(PUSHN);
	} break;

	case LT_AST_NODE_BINARYOP: {
		_lt_compile_node(vm, p, name, debug, node->binary_op.left, scope, code_body, constants);
		_lt_compile_node(vm, p, name, debug, node->binary_op.right, scope, code_body, constants);
		switch (node->binary_op.type)
		{
		case LT_TOKEN_PLUS: OP(ADD); break;
		case LT_TOKEN_MINUS: OP(SUB); break;
		case LT_TOKEN_MULTIPLY: OP(MUL); break;
		case LT_TOKEN_DIVIDE: OP(DIV); break;
		case LT_TOKEN_AND: OP(AND); break;
		case LT_TOKEN_OR: OP(OR); break;
		case LT_TOKEN_EQUALS: OP(EQ); break;
		case LT_TOKEN_NOTEQUALS: OP(NEQ); break;
		case LT_TOKEN_GT: OP(GT); break;
		case LT_TOKEN_GTE: OP(GTE); break;
		}
	} break;

	case LT_AST_NODE_UNARYOP: {
		_lt_compile_node(vm, p, name, debug, node->unary_op.expr, scope, code_body, constants);
		switch (node->unary_op.type)
		{
		case LT_TOKEN_NEGATE: OP(NEG); break;
		case LT_TOKEN_NOT: OP(NOT); break;
		case LT_TOKEN_TYPE: OP(TYPE); break;
		case LT_TOKEN_TYPEOF: OP(TYPEOF); break;
		}
	} break;

	case LT_AST_NODE_DECLARE: {
		if (node->declare.is_global)
		{
			if (node->declare.expr) _lt_compile_node(vm, p, name, debug, node->declare.expr, scope, code_body, constants);
			else OP(PUSHN);
			_lt_compile_push_token_value(vm, p, node->declare.identifier, debug, code_body, constants, &node->loc);
			OP(SETG);
		}
		else if (node->declare.destructure == LT_DESTRUCT_NONE)
		{
			uint16_t idx = lt_make_local_internal(vm, scope, node->declare.identifier);
			if (node->declare.expr)
			{
				_lt_compile_node(vm, p, name, debug, node->declare.expr, scope, code_body, constants);
				if (lt_is_captured_local_internal(scope, idx)) OPARG(STORECELL, idx)
				else OPARG(STORE, idx);
			}
		}
		else
		{
			uint8_t rhs_is_call = node->declare.expr && node->declare.expr->type == LT_AST_NODE_CALL;
			_lt_compile_node_ex(vm, p, name, debug, node->declare.expr, scope, code_body, constants, rhs_is_call);
			if (rhs_is_call) OP(PACKRET);

			for (uint32_t i = 0; i < node->declare.entries.length; ++i)
			{
				lt_DestructureEntry* entry = lt_buffer_at(&node->declare.entries, i);
				OP(DUP);
				if (node->declare.destructure == LT_DESTRUCT_ARRAY)
					_lt_compile_push_token_value(vm, p, lt_make_number_token_internal(vm, p, (double)i, entry->local), debug, code_body, constants, &node->loc);
				else
					_lt_compile_push_token_value(vm, p, entry->key, debug, code_body, constants, &node->loc);
				OP(GETD);
				uint32_t idx = lt_find_local_internal(vm, scope, entry->local);
				if (lt_is_captured_local_internal(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
				else OPARG(STORE, idx & 0xFFFF);
			}
			OPARG(POP, 1);
		}
	} break;

	case LT_AST_NODE_CLASS: {
		lt_Value class_name = lt_make_string(vm, ((lt_Identifier*)lt_buffer_at(&p->tkn->identifier_buffer, node->class_decl.identifier->idx))->name);
		uint32_t idx = lt_find_local_internal(vm, scope, node->class_decl.identifier);
		if (idx == NOT_FOUND) idx = lt_make_local_internal(vm, scope, node->class_decl.identifier);

		OPARG(PUSHC, _lt_push_constant(vm, constants, class_name));
		OP(MAKEC);
		if (lt_is_captured_local_internal(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
		else OPARG(STORE, idx & 0xFFFF);

		if (node->class_decl.superclass)
		{
			if (lt_is_captured_local_internal(scope, idx)) OPARG(LOADCELL, idx & 0xFFFF)
			else OPARG(LOAD, idx & 0xFFFF);
			uint32_t super_idx = lt_find_local_internal(vm, scope, node->class_decl.superclass);
			if (super_idx == NOT_FOUND) lt_parse_error_internal(vm, name, node->class_decl.superclass, "Can't find superclass!");
			else if ((super_idx & UPVAL_BIT) == UPVAL_BIT) OPARG(LOADUP, super_idx & 0xFFFF)
			else if (lt_is_captured_local_internal(scope, super_idx)) OPARG(LOADCELL, super_idx & 0xFFFF)
			else OPARG(LOAD, super_idx & 0xFFFF);
			OP(SETSUPER);
		}

		for (uint32_t i = 0; i < node->class_decl.members.length; ++i)
		{
			lt_ClassMember* member = lt_buffer_at(&node->class_decl.members, i);
			lt_Value member_name = member->type == LT_CLASS_CONSTRUCTOR
				? lt_make_string(vm, "constructor")
				: lt_make_string(vm, ((lt_Identifier*)lt_buffer_at(&p->tkn->identifier_buffer, member->name->idx))->name);

			_lt_compile_function_capture(vm, p, name, debug, member->value, scope, code_body, constants, 0, 1);
			if (lt_is_captured_local_internal(scope, idx)) OPARG(LOADCELL, idx & 0xFFFF)
			else OPARG(LOAD, idx & 0xFFFF);
			OPARG(PUSHC, _lt_push_constant(vm, constants, member_name));
			OPARG(SETC, _lt_encode_class_member(member));
		}
	} break;

	case LT_AST_NODE_ASSIGN: {
		lt_AstNode* target = node->assign.left;
		if (target->type == LT_AST_NODE_IDENTIFIER)
		{
			_lt_compile_node(vm, p, name, debug, node->assign.right, scope, code_body, constants);
			uint32_t idx = lt_find_local_internal(vm, scope, target->identifier.token);
			if (idx == NOT_FOUND) lt_parse_error_internal(vm, name, target->identifier.token, "Can't find local to assign to!");
			else if ((idx & UPVAL_BIT) == UPVAL_BIT) OPARG(STOREUP, idx & 0xFFFF)
			else if (lt_is_captured_local_internal(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
			else OPARG(STORE, idx & 0xFFFF);
		}
		else if (target->type == LT_AST_NODE_INDEX)
		{
			_lt_compile_index(vm, p, name, debug, target, scope, code_body, constants);
			_lt_compile_node(vm, p, name, debug, node->assign.right, scope, code_body, constants);
			OP(SETT);
		}
	} break;

	case LT_AST_NODE_FN: {
		lt_Value as_val = _lt_compile_function_value(vm, p, name, node, 0);
		uint16_t idx = _lt_push_constant(vm, constants, as_val);
		OPARG(PUSHC, idx);

		if (node->fn.scope->upvals.length > 0)
		{
			lt_Buffer* upvals = &node->fn.scope->upvals;
			// this is actually a closure

			for (int i = upvals->length - 1; i >= 0; i--)
			{
				uint32_t idx = lt_find_local_internal(vm, scope, (lt_Token*)lt_buffer_at(upvals, i));
				if ((idx & UPVAL_BIT) == UPVAL_BIT) OPARG(LOADUPCELL, idx & 0xFFFF)
				else OPARG(CAPTURE, idx & 0xFFFF);
			}

			OPARG(CLOSE, upvals->length);
		}
	} break;

	case LT_AST_NODE_CALL: {
		lt_AstNode** arg = node->call.args;
		uint8_t narg = 0;
		uint8_t total = 0;
		while (node->call.args[total]) total++;
		while (*arg)
		{
			uint8_t is_last = narg == total - 1;
			uint8_t arg_multi = is_last && (*arg)->type == LT_AST_NODE_CALL;
			_lt_compile_node_ex(vm, p, name, debug, *arg++, scope, code_body, constants, arg_multi);
			narg++;
		}

		if (node->call.callee->type == LT_AST_NODE_SUPER)
		{
			if (node->call.callee->super_expr.method)
			{
				_lt_compile_push_token_value(vm, p, node->call.callee->super_expr.method, debug, code_body, constants, &node->loc);
				OPARG(SUPERM, narg);
			}
			else OPARG(SUPERC, narg);
		}
		else
		{
			_lt_compile_node(vm, p, name, debug, node->call.callee, scope, code_body, constants);
			if (total > 0 && node->call.args[total - 1]->type == LT_AST_NODE_CALL) OPARG(CALLM, narg - 1)
			else OPARG(CALL, narg);
		}
		if (!allow_multi) OPARG(FIXRET, 1);
	} break;

	case LT_AST_NODE_AWAIT: {
		_lt_compile_node(vm, p, name, debug, node->await.expr, scope, code_body, constants);
		OP(AWAIT);
	} break;

	case LT_AST_NODE_RETURN: {
		uint8_t returns_many = 0;
		if (node->ret.expr)
		{
			if (node->ret.expr->type == LT_AST_NODE_CALL)
			{
				_lt_compile_node_ex(vm, p, name, debug, node->ret.expr, scope, code_body, constants, 1);
				returns_many = 1;
			}
			else
			{
				_lt_compile_node(vm, p, name, debug, node->ret.expr, scope, code_body, constants);
			}
		}

		for (int32_t i = (int32_t)node->ret.cleanups.length - 1; i >= 0; --i)
		{
			uint16_t sourceidx = *(uint16_t*)lt_buffer_at(&node->ret.cleanups, i);
			OPARG(LOAD, sourceidx);
			OP(ACLOSE);
		}

		if (returns_many) { OP(RETM); }
		else if (node->ret.expr) { OPARG(RET, 1); }
		else { OP(RET); }
	} break;

#define REG_JMP() { if (n_branches >= LT_MAX_BRANCHES) lt_error(vm, "Too many if/elseif branches!"); branch_stack[n_branches++] = code_body->length; OP(NOP); }

	case LT_AST_NODE_IF: {
		uint8_t n_branches = 0;
		uint32_t branch_stack[LT_MAX_BRANCHES];

		_lt_compile_node(vm, p, name, debug, node->branch.expr, scope, code_body, constants);
		uint32_t jidx = code_body->length;
		OP(JMPC);

		_lt_compile_body(vm, p, name, debug, &node->branch.body, node->branch.scope, code_body, constants);
		REG_JMP();

		((lt_Op*)lt_buffer_at(code_body, jidx))->arg = code_body->length - jidx - 1;

		uint8_t has_elseif = 0, has_else = 0;

		lt_AstNode* next = node->branch.next;
		while (next)
		{
			if (next->type == LT_AST_NODE_ELSEIF)
			{
				has_elseif = 1;

				if (has_else) lt_error(vm, "'else' must be last in if-chain!");

				_lt_compile_node(vm, p, name, debug, next->branch.expr, scope, code_body, constants);
				uint32_t jidx = code_body->length;
				OP(JMPC);

				_lt_compile_body(vm, p, name, debug, &next->branch.body, next->branch.scope, code_body, constants);
				REG_JMP();

				((lt_Op*)lt_buffer_at(code_body, jidx))->arg = code_body->length - jidx - 1;
			}
			else
			{
				has_else = 1;

				_lt_compile_body(vm, p, name, debug, &next->branch.body, next->branch.scope, code_body, constants);
			}

			next = next->branch.next;
		}

		if (has_elseif || has_else)
		{
			for (uint32_t i = 0; i < n_branches; i++)
			{
				uint32_t loc = branch_stack[i];
				uint32_t arg = code_body->length - loc - 1;
				/* A jump to the next instruction is a no-op. Emitting NOP keeps
				   this from looking like an unresolved `break` placeholder, which
				   the enclosing loop's post-pass would rewrite to jump past the
				   loop. */
				*((lt_Op*)lt_buffer_at(code_body, loc)) = (arg == 0)
					? (lt_Op) { LT_OP_NOP, 0 }
					: (lt_Op) { LT_OP_JMP, arg };
			}
		}
	} break;

	case LT_AST_NODE_FOR: {
		_lt_compile_node(vm, p, name, debug, node->loop.iterator, scope, code_body, constants);
		if (node->loop.is_async)
		{
			OP(AWAIT);
			OPARG(STORE, node->loop.sourceidx);
			OPARG(LOAD, node->loop.sourceidx);
			OP(AITER);
		}
		else OPARG(ITER, node->loop.identifier_count);
		OPARG(STORE, node->loop.closureidx);

		uint32_t loop_header = code_body->length;
		OPARG(LOAD, node->loop.closureidx);
		OPARG(CALL, 0);
		uint32_t loop_start;
		if (node->loop.is_async)
		{
			OP(PACKRET);
			OP(AWAIT);
			OPARG(STORE, node->loop.itemidx);
			OPARG(LOAD, node->loop.itemidx);
			loop_start = code_body->length;
			OPARG(JMPN, 0);
			OPARG(LOAD, node->loop.itemidx);
			if (node->loop.identifier_count == 1)
			{
				uint16_t identifier = node->loop.identifier[0];
				if (lt_is_captured_local_internal(node->loop.scope, identifier)) OPARG(STORECELL, identifier)
				else OPARG(STORE, identifier);
			}
			else
			{
				OPARG(ABIND, 2);
				for (int32_t i = node->loop.identifier_count - 1; i >= 0; --i)
				{
					uint16_t identifier = node->loop.identifier[i];
					if (lt_is_captured_local_internal(node->loop.scope, identifier)) OPARG(STORECELL, identifier)
					else OPARG(STORE, identifier);
				}
			}
		}
		else
		{
			OPARG(FIXRET, node->loop.identifier_count);
			for (int32_t i = node->loop.identifier_count - 1; i >= 0; --i)
			{
				uint16_t identifier = node->loop.identifier[i];
				if (lt_is_captured_local_internal(node->loop.scope, identifier)) OPARG(STORECELL, identifier)
				else OPARG(STORE, identifier);
			}
			uint16_t first_identifier = node->loop.identifier[0];
			if (lt_is_captured_local_internal(node->loop.scope, first_identifier)) OPARG(LOADCELL, first_identifier)
			else OPARG(LOAD, first_identifier);
			loop_start = code_body->length;
			OPARG(JMPN, 0);
		}

		_lt_compile_body(vm, p, name, debug, &node->loop.body, node->loop.scope, code_body, constants);
		OPARG(JMP, loop_header - code_body->length - 1);

		uint32_t close_target = code_body->length;
		if (node->loop.is_async)
		{
			OPARG(LOAD, node->loop.sourceidx);
			OP(ACLOSE);
		}

		lt_Op* cond = lt_buffer_at(code_body, loop_start);
		cond->arg = close_target - loop_start - 1;

		for (uint32_t i = loop_start; i < close_target; ++i)
		{
			lt_Op* current = lt_buffer_at(code_body, i);
			if (current->op == LT_OP_BREAK)
			{
				/* Flip to JMP so an enclosing loop's pass cannot resolve it
				   again and redirect it past the outer loop. */
				current->op = LT_OP_JMP;
				current->arg = close_target - i - 1;
			}
		}
	} break;

	case LT_AST_NODE_WHILE: {
		uint32_t loop_header = code_body->length;
		_lt_compile_node(vm, p, name, debug, node->loop.iterator, scope, code_body, constants);
		uint32_t loop_start = code_body->length;
		OPARG(JMPC, 0);

		_lt_compile_body(vm, p, name, debug, &node->loop.body, node->loop.scope, code_body, constants);
		OPARG(JMP, loop_header - code_body->length - 1);

		lt_Op* cond = lt_buffer_at(code_body, loop_start);
		cond->arg = code_body->length - loop_start - 1;

		for (uint32_t i = loop_start; i < code_body->length; ++i)
		{
			lt_Op* current = lt_buffer_at(code_body, i);
			if (current->op == LT_OP_BREAK)
			{
				/* Flip to JMP so an enclosing loop's pass cannot resolve it
				   again and redirect it past the outer loop. */
				current->op = LT_OP_JMP;
				current->arg = code_body->length - i - 1;
			}
		}
	} break;

	case LT_AST_NODE_WITH: {
		_lt_compile_node(vm, p, name, debug, node->with_stmt.expr, scope, code_body, constants);
		uint32_t idx = lt_find_local_internal(vm, node->with_stmt.scope, node->with_stmt.receiver);
		if (idx == NOT_FOUND) idx = lt_make_local_internal(vm, node->with_stmt.scope, node->with_stmt.receiver);
		if (lt_is_captured_local_internal(node->with_stmt.scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
		else OPARG(STORE, idx & 0xFFFF);
		_lt_compile_body(vm, p, name, debug, &node->with_stmt.body, node->with_stmt.scope, code_body, constants);
	} break;
	}
}

static void _lt_compile_body(lt_VM* vm, lt_Parser* p, const char* name, lt_Buffer* debug, lt_Buffer* ast_body, lt_Scope* scope, lt_Buffer* code_body, lt_Buffer* constants)
{
	for (uint32_t i = 0; i < ast_body->length; i++)
	{
		lt_AstNode* node = *(lt_AstNode**)lt_buffer_at(ast_body, i);
		if (node->type == LT_AST_NODE_CALL)
			_lt_compile_node_ex(vm, p, name, debug, node, scope, code_body, constants, 1);
		else
			_lt_compile_node(vm, p, name, debug, node, scope, code_body, constants);
	}
}

lt_Value lt_compile(lt_VM* vm, lt_Parser* p)
{
	lt_Object* chunk = lt_allocate(vm, LT_OBJECT_CHUNK);
	lt_nocollect(vm, chunk);

	chunk->chunk.code = lt_buffer_new(sizeof(lt_Op));
	chunk->chunk.constants = lt_buffer_new(sizeof(lt_Value));

	if(p->tkn->module)
	{
		uint32_t len = (uint32_t)strlen(p->tkn->module);
		chunk->chunk.name = vm->alloc(len + 1);
		memcpy(chunk->chunk.name, p->tkn->module, len);
		chunk->chunk.name[len] = 0;
	}

	if (vm->generate_debug)
	{
		chunk->chunk.debug = vm->alloc(sizeof(lt_DebugInfo));
		chunk->chunk.debug->locations = lt_buffer_new(sizeof(lt_DebugLoc));
		chunk->chunk.debug->module_name = chunk->chunk.name;
	}

	lt_Buffer* debug = vm->generate_debug ? &chunk->chunk.debug->locations : 0;
	lt_Op op = { LT_OP_PUSH, 0 };
	lt_buffer_push(vm, &chunk->chunk.code, &op);
	if (debug) lt_buffer_push(vm, debug, &p->root->loc);

	_lt_compile_body(vm, p, chunk->chunk.name, debug, &p->root->chunk.body, p->root->chunk.scope, &chunk->chunk.code, &chunk->chunk.constants);

	lt_DebugLoc ret_loc = { 0, 0 };
	if (debug && debug->length > 0) ret_loc = *(lt_DebugLoc*)lt_buffer_at(debug, debug->length - 1);
	lt_Op op2 = { LT_OP_RET, 0 };
	lt_buffer_push(vm, &chunk->chunk.code, &op2);
	if (debug) lt_buffer_push(vm, debug, &ret_loc);

	((lt_Op*)lt_buffer_at(&chunk->chunk.code, 0))->arg = p->root->chunk.scope->frame_slots.length;

	lt_Value as_val = LT_VALUE_OBJECT(chunk);
	return as_val;
}
