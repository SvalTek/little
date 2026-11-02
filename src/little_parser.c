#include "little.h"
#include "little_buffer_internal.h"
#include "little_frontend_internal.h"
#include "little_value_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static lt_AstNode* _lt_get_node_of_type(lt_VM* vm, lt_Token* current, lt_Parser* p, lt_AstNodeType type)
{
	lt_AstNode* new_node = vm->alloc(sizeof(lt_AstNode));
	memset(new_node, 0, sizeof(lt_AstNode));
	new_node->type = type;
	new_node->loc.line = current->line;
	new_node->loc.col = current->col;
	new_node->span = current->span;

	lt_buffer_push(vm, &p->ast_nodes, &new_node);
	return *(void**)lt_buffer_last(&p->ast_nodes);
}

static uint32_t _lt_tokens_equal(lt_Token* a, lt_Token* b)
{
	return (a->type == LT_TOKEN_IDENTIFIER && b->type == LT_TOKEN_IDENTIFIER && a->idx == b->idx);
}

uint16_t lt_make_local_internal(lt_VM* vm, lt_Scope* scope, lt_Token* t)
{
	for (uint32_t i = 0; i < scope->locals.length; ++i)
	{
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&scope->locals, i), t))
			return *(uint16_t*)lt_buffer_at(&scope->local_slots, i);
	}

	lt_Scope* frame = scope->frame;
	if (frame->frame_slots.length >= LT_MAX_LOCALS) lt_error(vm, "Too many local variables!");
	uint16_t slot = (uint16_t)frame->frame_slots.length;
	lt_buffer_push(vm, &frame->frame_slots, &t);
	lt_buffer_push(vm, &scope->locals, t);
	lt_buffer_push(vm, &scope->local_slots, &slot);
	return slot;
}

static void _lt_mark_captured_local(lt_VM* vm, lt_Scope* frame, uint16_t slot)
{
	for (uint32_t i = 0; i < frame->captured.length; ++i)
		if (*(uint16_t*)lt_buffer_at(&frame->captured, i) == slot) return;
	lt_buffer_push(vm, &frame->captured, &slot);
}

#define UPVAL_BIT 0x07000000
#define NOT_FOUND ((uint32_t)-1)

uint8_t lt_is_captured_local_internal(lt_Scope* scope, uint32_t idx)
{
	lt_Scope* frame = scope->frame;
	if ((idx & UPVAL_BIT) == UPVAL_BIT || idx >= frame->frame_slots.length) return 0;
	for (uint32_t i = 0; i < frame->captured.length; ++i)
		if (*(uint16_t*)lt_buffer_at(&frame->captured, i) == idx) return 1;
	return 0;
}


uint32_t lt_find_local_internal(lt_VM* vm, lt_Scope* scope, lt_Token* t)
{
	lt_Scope* frame = scope->frame;
	for (lt_Scope* lexical = scope; lexical && lexical->frame == frame; lexical = lexical->last)
	{
		for (uint32_t i = 0; i < lexical->locals.length; ++i)
			if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&lexical->locals, i), t))
				return *(uint16_t*)lt_buffer_at(&lexical->local_slots, i);
	}

	for (uint32_t i = 0; i < frame->upvals.length; ++i)
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&frame->upvals, i), t)) return i | UPVAL_BIT;

	lt_Scope* test = frame->last;
	while (test)
	{
		uint32_t found_slot = NOT_FOUND;
		for (uint32_t i = 0; i < test->locals.length; ++i)
		{
			if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&test->locals, i), t))
			{
				found_slot = *(uint16_t*)lt_buffer_at(&test->local_slots, i);
				break;
			}
		}

		if (found_slot != NOT_FOUND)
		{
			_lt_mark_captured_local(vm, test->frame, (uint16_t)found_slot);
			for (uint32_t i = 0; i < frame->upvals.length; ++i)
			{
				if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&frame->upvals, i), t))
					return i | UPVAL_BIT;
			}
			lt_buffer_push(vm, &frame->upvals, t);
			return (frame->upvals.length - 1) | UPVAL_BIT;
		}

		if (test == test->frame)
		{
			for (uint32_t i = 0; i < test->upvals.length; ++i)
			{
				if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&test->upvals, i), t))
				{
					for (uint32_t j = 0; j < frame->upvals.length; ++j)
						if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&frame->upvals, j), t)) return j | UPVAL_BIT;
					lt_buffer_push(vm, &frame->upvals, t);
					return (frame->upvals.length - 1) | UPVAL_BIT;
				}
			}
		}

		test = test->last;
	}

	return NOT_FOUND;
}

static lt_Token* _lt_parse_expression(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_AstNode* dst);
static lt_Token* _lt_make_identifier_token(lt_VM* vm, lt_Parser* p, const char* name, lt_Token* loc)
{
	uint16_t len = (uint16_t)strlen(name);
	uint32_t idx = p->tkn->identifier_buffer.length;
	for (uint32_t i = 0; i < p->tkn->identifier_buffer.length; ++i)
	{
		lt_Identifier* existing = lt_buffer_at(&p->tkn->identifier_buffer, i);
		if (faststrcmp(name, len, existing->name, strlen(existing->name)))
		{
			idx = i;
			break;
		}
	}

	if (idx == p->tkn->identifier_buffer.length)
	{
		lt_Identifier newid;
		newid.num_references = 1;
		newid.name = vm->alloc(len + 1);
		strncpy_s(newid.name, len + 1, name, len);
		newid.name[len] = 0;
		lt_buffer_push(vm, &p->tkn->identifier_buffer, &newid);
	}

	lt_Token* tok = vm->alloc(sizeof(lt_Token));
	tok->type = LT_TOKEN_IDENTIFIER;
	tok->line = loc->line;
	tok->col = loc->col;
	tok->idx = idx;
	tok->flags = LT_TOKEN_SYNTHETIC;
	tok->span = loc->span;
	lt_buffer_push(vm, &p->generated_tokens, &tok);
	return tok;
}

lt_Token* lt_make_number_token_internal(lt_VM* vm, lt_Parser* p, double number, lt_Token* loc)
{
	lt_Literal literal;
	literal.type = LT_TOKEN_NUMBER_LITERAL;
	literal.number = number;
	lt_buffer_push(vm, &p->tkn->literal_buffer, &literal);

	lt_Token* tok = vm->alloc(sizeof(lt_Token));
	tok->type = LT_TOKEN_NUMBER_LITERAL;
	tok->line = loc->line;
	tok->col = loc->col;
	tok->idx = p->tkn->literal_buffer.length - 1;
	tok->flags = LT_TOKEN_SYNTHETIC;
	tok->span = loc->span;
	lt_buffer_push(vm, &p->generated_tokens, &tok);
	return tok;
}

static lt_Token* _lt_make_hidden_identifier_token(lt_VM* vm, lt_Parser* p, const char* prefix, lt_Token* loc)
{
	char name[32];
	sprintf_s(name, sizeof(name), "%s%d", prefix, p->next_with_id++);
	return _lt_make_identifier_token(vm, p, name, loc);
}

static lt_AstNode* _lt_make_identifier_node(lt_VM* vm, lt_Parser* p, lt_Token* loc, lt_Token* token)
{
	lt_AstNode* ident = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_IDENTIFIER);
	ident->identifier.token = token;
	return ident;
}

static lt_AstNode* _lt_make_import_call_node(lt_VM* vm, lt_Parser* p, lt_Token* loc, lt_AstNode* path_expr)
{
	lt_AstNode* call = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_CALL);
	call->call.callee = _lt_make_identifier_node(vm, p, loc, _lt_make_identifier_token(vm, p, "import", loc));
	call->call.args[0] = path_expr;
	return call;
}

static lt_AstNode* _lt_make_self_index_node(lt_VM* vm, lt_Parser* p, lt_Token* loc, lt_Token* field)
{
	lt_Token* self = p->self_token;
	if (!self)
	{
		lt_Token* this_token = _lt_make_identifier_token(vm, p, "this", loc);
		if (lt_find_local_internal(vm, p->current, this_token) != NOT_FOUND) self = this_token;
	}
	if (!self) lt_parse_error_internal(vm, p->tkn->module, loc, "'@' is only valid where 'this' is available!");

	lt_AstNode* idx_expr = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_LITERAL);
	idx_expr->literal.token = field;

	lt_AstNode* index = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_INDEX);
	index->index.source = _lt_make_identifier_node(vm, p, loc, self);
	index->index.idx = idx_expr;
	return index;
}

static lt_AstNode* _lt_make_self_assignment_node(lt_VM* vm, lt_Parser* p, lt_Token* loc, lt_Token* self, lt_Token* field, lt_Token* value)
{
	lt_Token* previous_self = p->self_token;
	p->self_token = self;

	lt_AstNode* assign = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_ASSIGN);
	assign->assign.left = _lt_make_self_index_node(vm, p, loc, field);
	assign->assign.right = _lt_make_identifier_node(vm, p, loc, value);

	p->self_token = previous_self;
	return assign;
}

static void _lt_parse_soft_error(lt_VM* vm, lt_Parser* p, lt_Token* t, const char* message)
{
	lt_push_diagnostic_internal(vm, t->span, t->line, t->col, LT_DIAG_ERROR, message);

	char sprint_buf[128];
	sprintf_s(sprint_buf, 128, "%s|%d:%d: %s", p->tkn->module, t->line, t->col, message);
	if (vm->error) vm->error(vm, sprint_buf);
	p->had_error = 1;
}

static uint8_t _lt_class_member_name_equals(lt_ClassMember* member, lt_Token* name)
{
	return member->name && member->name->type == LT_TOKEN_IDENTIFIER && name->type == LT_TOKEN_IDENTIFIER && member->name->idx == name->idx;
}

static uint8_t _lt_class_has_member_kind(lt_Buffer* members, lt_Token* name, lt_ClassMemberType type, lt_Visibility visibility)
{
	for (uint32_t i = 0; i < members->length; ++i)
	{
		lt_ClassMember* member = lt_buffer_at(members, i);
		if (member->type == type && member->visibility == visibility && _lt_class_member_name_equals(member, name)) return 1;
	}
	return 0;
}

static uint8_t _lt_class_has_field(lt_Buffer* members, lt_Token* name)
{
	for (uint32_t i = 0; i < members->length; ++i)
	{
		lt_ClassMember* member = lt_buffer_at(members, i);
		if (!_lt_class_member_name_equals(member, name)) continue;
		if (member->type == LT_CLASS_FIELD) return 1;
	}
	return 0;
}

static uint8_t _lt_class_has_accessor(lt_Buffer* members, lt_Token* name)
{
	for (uint32_t i = 0; i < members->length; ++i)
	{
		lt_ClassMember* member = lt_buffer_at(members, i);
		if (!_lt_class_member_name_equals(member, name)) continue;
		if (member->type == LT_CLASS_GETTER || member->type == LT_CLASS_SETTER) return 1;
	}
	return 0;
}

static uint8_t _lt_class_has_conflicting_member(lt_Buffer* members, lt_Token* name, lt_ClassMemberType type)
{
	for (uint32_t i = 0; i < members->length; ++i)
	{
		lt_ClassMember* member = lt_buffer_at(members, i);
		if (!_lt_class_member_name_equals(member, name)) continue;
		if ((type == LT_CLASS_GETTER && member->type == LT_CLASS_SETTER) ||
			(type == LT_CLASS_SETTER && member->type == LT_CLASS_GETTER))
			continue;
		return 1;
	}
	return 0;
}

static lt_AstNode* _lt_make_return_node(lt_VM* vm, lt_Parser* p, lt_Token* loc, lt_AstNode* expr)
{
	lt_AstNode* ret = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_RETURN);
	ret->ret.expr = expr;
	return ret;
}

static lt_AstNode* _lt_make_field_initializer_fn(lt_VM* vm, lt_Parser* p, lt_Token* loc, lt_AstNode* expr)
{
	lt_AstNode* fn = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_FN);
	fn->fn.args[0] = _lt_make_identifier_token(vm, p, "this", loc);
	fn->fn.body = lt_buffer_new(sizeof(lt_AstNode*));
	if (expr)
	{
		lt_AstNode* ret = _lt_make_return_node(vm, p, loc, expr);
		lt_buffer_push(vm, &fn->fn.body, &ret);
	}

	lt_Scope* fn_scope = vm->alloc(sizeof(lt_Scope));
	fn_scope->last = p->current;
	fn_scope->frame = fn_scope;
	fn_scope->start = loc;
	fn_scope->end = loc;
	fn_scope->locals = lt_buffer_new(sizeof(lt_Token));
	fn_scope->local_slots = lt_buffer_new(sizeof(uint16_t));
	fn_scope->frame_slots = lt_buffer_new(sizeof(lt_Token*));
	fn_scope->upvals = lt_buffer_new(sizeof(lt_Token));
	fn_scope->captured = lt_buffer_new(sizeof(uint16_t));
	lt_buffer_push(vm, &p->scopes, &fn_scope);
	lt_make_local_internal(vm, fn_scope, fn->fn.args[0]);
	fn->fn.scope = fn_scope;
	return fn;
}

static lt_Token* _lt_parse_class_function(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* fn, uint8_t add_this, uint8_t allow_auto_assign, uint8_t is_constructor);
static lt_Token* _lt_parse_destructure_pattern(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* declare);

static lt_Token* _lt_parse_var_declaration(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* declare, uint8_t is_global)
{
	if (current->type == LT_TOKEN_IDENTIFIER)
	{
		declare->declare.identifier = current++;
		if (!is_global) lt_make_local_internal(vm, p->current, declare->declare.identifier);
	}
	else if (!is_global && (current->type == LT_TOKEN_OPENBRACKET || current->type == LT_TOKEN_OPENBRACE))
	{
		current = _lt_parse_destructure_pattern(vm, p, current, declare);
	}
	else lt_parse_error_internal(vm, p->tkn->module, current, is_global ? "Expected identifier to follow 'global'!" : "Expected identifier or destructuring pattern to follow 'var'!");

	lt_AstNode* rhs = 0;
	if (current->type == LT_TOKEN_ASSIGN)
	{
		current++;
		rhs = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
		current = _lt_parse_expression(vm, p, current, rhs);
	}
	else if (declare->declare.destructure != LT_DESTRUCT_NONE)
		_lt_parse_soft_error(vm, p, current, "Expected assignment after destructuring declaration!");

	declare->declare.expr = rhs;
	declare->declare.is_global = is_global;
	return current;
}

static uint8_t _lt_token_is_name(lt_Parser* p, lt_Token* token, const char* name)
{
	if (token->type != LT_TOKEN_IDENTIFIER) return 0;
	lt_Identifier* id = lt_buffer_at(&p->tkn->identifier_buffer, token->idx);
	return faststrcmp(name, strlen(name), id->name, strlen(id->name));
}

uint8_t lt_token_is_keyword_internal(lt_Token* token)
{
	return token->type >= LT_TOKEN_FN && token->type <= LT_TOKEN_RETURN;
}

static lt_Token* _lt_keyword_as_identifier(lt_VM* vm, lt_Parser* p, lt_Token* keyword)
{
	lt_Token* ident = vm->alloc(sizeof(lt_Token));
	*ident = *keyword;
	ident->type = LT_TOKEN_IDENTIFIER;
	lt_buffer_push(vm, &p->generated_tokens, &ident);
	return ident;
}

static lt_Token* _lt_parse_named_function_declaration(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* declare, uint8_t is_global, uint8_t is_async)
{
	lt_Token* loc = current;
	if (is_async)
	{
		if (current->type != LT_TOKEN_ASYNC) lt_parse_error_internal(vm, p->tkn->module, current, "Expected 'async' in async function declaration!");
		current++;
		if (current->type != LT_TOKEN_FN) lt_parse_error_internal(vm, p->tkn->module, current, "Expected 'fn' to follow 'async'!");
	}
	else if (current->type != LT_TOKEN_FN)
		lt_parse_error_internal(vm, p->tkn->module, current, "Expected 'fn' in function declaration!");

	current++;
	if (current->type != LT_TOKEN_IDENTIFIER) lt_parse_error_internal(vm, p->tkn->module, current, is_global ? "Expected function name to follow 'global fn'!" : "Expected function name to follow 'fn'!");
	declare->declare.identifier = current++;
	if (!is_global) lt_make_local_internal(vm, p->current, declare->declare.identifier);

	lt_AstNode* func = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_FN);
	func->fn.is_async = is_async;
	current = _lt_parse_class_function(vm, p, current, func, 0, 0, 0);
	declare->declare.expr = func;
	declare->declare.is_global = is_global;
	return current;
}

static lt_Token* _lt_skip_destructure_pattern(lt_Token* current, lt_TokenType close_type)
{
	while (current->type != LT_TOKEN_END && current->type != close_type) current++;
	if (current->type == close_type) return current + 1;
	return current;
}

static uint8_t _lt_is_postfix_source(lt_Token* last)
{
	if (!last) return 0;
	switch (last->type)
	{
	case LT_TOKEN_CLOSEBRACE:
	case LT_TOKEN_CLOSEBRACKET:
	case LT_TOKEN_CLOSEPAREN:
	case LT_TOKEN_IDENTIFIER:
		return 1;
	default:
		return 0;
	}
}

static lt_Token* _lt_parse_destructure_pattern(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* declare)
{
	declare->declare.entries = lt_buffer_new(sizeof(lt_DestructureEntry));

	if (current->type == LT_TOKEN_OPENBRACKET)
	{
		declare->declare.destructure = LT_DESTRUCT_ARRAY;
		current++;
		while (current->type != LT_TOKEN_CLOSEBRACKET)
		{
			if (current->type == LT_TOKEN_END)
			{
				_lt_parse_soft_error(vm, p, current, "Unexpected end of file in array destructuring pattern!");
				return current;
			}
			if (current->type != LT_TOKEN_IDENTIFIER)
			{
				_lt_parse_soft_error(vm, p, current, "Expected identifier in array destructuring pattern!");
				return _lt_skip_destructure_pattern(current, LT_TOKEN_CLOSEBRACKET);
			}
			lt_DestructureEntry entry = { 0 };
			entry.local = current++;
			lt_make_local_internal(vm, p->current, entry.local);
			lt_buffer_push(vm, &declare->declare.entries, &entry);
			if (current->type == LT_TOKEN_COMMA) current++;
			else if (current->type != LT_TOKEN_CLOSEBRACKET)
			{
				_lt_parse_soft_error(vm, p, current, "Expected comma or closing bracket in array destructuring pattern!");
				return _lt_skip_destructure_pattern(current, LT_TOKEN_CLOSEBRACKET);
			}
		}
		return current + 1;
	}

	if (current->type == LT_TOKEN_OPENBRACE)
	{
		declare->declare.destructure = LT_DESTRUCT_TABLE;
		current++;
		while (current->type != LT_TOKEN_CLOSEBRACE)
		{
			if (current->type == LT_TOKEN_END)
			{
				_lt_parse_soft_error(vm, p, current, "Unexpected end of file in table destructuring pattern!");
				return current;
			}
			if (current->type != LT_TOKEN_IDENTIFIER)
			{
				_lt_parse_soft_error(vm, p, current, "Expected identifier key in table destructuring pattern!");
				return _lt_skip_destructure_pattern(current, LT_TOKEN_CLOSEBRACE);
			}
			lt_DestructureEntry entry = { 0 };
			entry.key = current++;
			entry.local = entry.key;
			if (current->type == LT_TOKEN_COLON)
			{
				current++;
				if (current->type != LT_TOKEN_IDENTIFIER)
				{
					_lt_parse_soft_error(vm, p, current, "Expected local identifier after ':' in table destructuring pattern!");
					return _lt_skip_destructure_pattern(current, LT_TOKEN_CLOSEBRACE);
				}
				entry.local = current++;
			}
			lt_make_local_internal(vm, p->current, entry.local);
			lt_buffer_push(vm, &declare->declare.entries, &entry);
			if (current->type == LT_TOKEN_COMMA) current++;
			else if (current->type != LT_TOKEN_CLOSEBRACE)
			{
				_lt_parse_soft_error(vm, p, current, "Expected comma or closing brace in table destructuring pattern!");
				return _lt_skip_destructure_pattern(current, LT_TOKEN_CLOSEBRACE);
			}
		}
		return current + 1;
	}

	return current;
}

enum { LT_PARSE_SCOPE_NONE, LT_PARSE_SCOPE_FRAME, LT_PARSE_SCOPE_LEXICAL };

static lt_Scope* _lt_parse_block(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_Buffer* dst, uint8_t expects_terminator, uint8_t scope_type, lt_Token** argnames);

static lt_Token* _lt_parse_for_statement(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_Buffer* dst, uint8_t is_async)
{
	lt_AstNode* for_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FOR);
	for_expr->loop.is_async = is_async;
	for_expr->loop.sourceidx = UINT16_MAX;
	for_expr->loop.itemidx = UINT16_MAX;
	current++; // eat for

	if (is_async && !p->in_async)
		lt_parse_error_internal(vm, p->tkn->module, current, "'async for' is only valid inside async functions!");

	lt_Token* identifiers[LT_MAX_LOOP_VARS];
	uint8_t identifier_count = 0;
	for (;;)
	{
		if (current->type != LT_TOKEN_IDENTIFIER)
			lt_parse_error_internal(vm, p->tkn->module, current, identifier_count == 0
				? "Expected identifier to follow 'for'!"
				: "Expected identifier after ',' in 'for' header!");
		for (uint8_t i = 0; i < identifier_count; ++i)
			if (_lt_tokens_equal(identifiers[i], current))
				lt_parse_error_internal(vm, p->tkn->module, current, "Duplicate loop variable!");
		identifiers[identifier_count++] = current++;

		if (current->type == LT_TOKEN_COMMA)
		{
			current++;
			if (identifier_count >= LT_MAX_LOOP_VARS)
				lt_parse_error_internal(vm, p->tkn->module, current, "Too many loop variables!");
			continue;
		}
		if (current->type == LT_TOKEN_IN) break;
		lt_parse_error_internal(vm, p->tkn->module, current, "Expected comma or 'in' after loop variable!");
	}
	current++;

	lt_AstNode* iter_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
	uint8_t was_allow_table_call = p->allow_table_call;
	p->allow_table_call = 0;
	current = _lt_parse_expression(vm, p, current, iter_expr);
	p->allow_table_call = was_allow_table_call;

	lt_Token* iter_token = _lt_make_hidden_identifier_token(vm, p, "__iter", identifiers[0]);
	for_expr->loop.closureidx = lt_make_local_internal(vm, p->current, iter_token);
	if (is_async)
	{
		lt_Token* source_token = _lt_make_hidden_identifier_token(vm, p, "__source", identifiers[0]);
		lt_Token* item_token = _lt_make_hidden_identifier_token(vm, p, "__item", identifiers[0]);
		for_expr->loop.sourceidx = lt_make_local_internal(vm, p->current, source_token);
		for_expr->loop.itemidx = lt_make_local_internal(vm, p->current, item_token);
	}
	for_expr->loop.iterator = iter_expr;

	if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow 'for' header!");
	current++;

	lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
	lt_Token* body_args[LT_MAX_LOOP_VARS + 1] = { 0 };
	for (uint8_t i = 0; i < identifier_count; ++i) body_args[i] = identifiers[i];
	uint32_t cleanup_len = p->async_cleanups.length;
	if (is_async) lt_buffer_push(vm, &p->async_cleanups, &for_expr->loop.sourceidx);
	lt_Scope* body_scope = _lt_parse_block(vm, p, current, &body, 1, LT_PARSE_SCOPE_LEXICAL, body_args);
	p->async_cleanups.length = cleanup_len;
	current = body_scope->end;

	for_expr->loop.identifier_count = identifier_count;
	for (uint8_t i = 0; i < identifier_count; ++i)
		for_expr->loop.identifier[i] = (uint16_t)lt_find_local_internal(vm, body_scope, identifiers[i]);
	for_expr->loop.body = body;
	for_expr->loop.scope = body_scope;

	lt_buffer_push(vm, dst, &for_expr);
	return current;
}

static lt_Scope* _lt_parse_block(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_Buffer* dst, uint8_t expects_terminator, uint8_t scope_type, lt_Token** argnames)
{
	if (scope_type != LT_PARSE_SCOPE_NONE)
	{
		lt_Scope* new_scope = vm->alloc(sizeof(lt_Scope));
		new_scope->last = p->current;
		new_scope->frame = scope_type == LT_PARSE_SCOPE_FRAME ? new_scope : p->current->frame;
		p->current = new_scope;

		new_scope->start = start;
		new_scope->end = start;

		new_scope->locals = lt_buffer_new(sizeof(lt_Token));
		new_scope->local_slots = lt_buffer_new(sizeof(uint16_t));
		new_scope->frame_slots = lt_buffer_new(sizeof(lt_Token*));
		new_scope->upvals = lt_buffer_new(sizeof(lt_Token));
		new_scope->captured = lt_buffer_new(sizeof(uint16_t));
		lt_buffer_push(vm, &p->scopes, &new_scope);

		if (argnames) while (*argnames) lt_make_local_internal(vm, p->current, *argnames++);
	}

	lt_Token* current = start;
	void* block_error_buf = vm->error_buf;

#define PEEK() (current + 1)
#define NEXT() (last = current, current++)
#define REQUIRE_STATEMENT_BOUNDARY() \
	if (current->type != LT_TOKEN_END && current->type != LT_TOKEN_CLOSEBRACE && current > start && (current - 1)->line == current->line) \
		lt_parse_error_internal(vm, p->tkn->module, current, "Expected newline between statements!")

	while (current->type != LT_TOKEN_END)
	{
		/* A broken statement must not abort the rest of the file. Trap the
		   parser error, restore the statement's state, skip past the broken
		   statement and continue with the next one. */
		lt_Token* statement_start = current;
		lt_Scope* statement_scope = p->current;
		uint8_t statement_async = p->in_async;
		uint8_t statement_constructor = p->in_constructor;
		uint8_t statement_table_call = p->allow_table_call;
		lt_Token* statement_self = p->self_token;
		uint32_t statement_cleanup_len = p->async_cleanups.length;
		uint32_t statement_cleanup_floor = p->async_cleanup_floor;

		void* saved_error_buf = vm->error_buf;
		jmp_buf recovery;
		vm->error_buf = &recovery;

		if (setjmp(recovery))
		{
			vm->error_buf = saved_error_buf;
			p->had_error = 1;
			p->current = statement_scope;
			p->in_async = statement_async;
			p->in_constructor = statement_constructor;
			p->allow_table_call = statement_table_call;
			p->self_token = statement_self;
			p->async_cleanups.length = statement_cleanup_len;
			p->async_cleanup_floor = statement_cleanup_floor;

			uint16_t error_line = vm->error_line ? vm->error_line : statement_start->line;
			uint16_t error_col = vm->error_col;

			/* Errors are often discovered by looking at the first token of the
			   next statement. Do not consume that line: it may still be valid.
			   Resync at the end of the statement that failed instead. */
			if (vm->error_at_token && error_line > statement_start->line)
			{
				uint16_t first_col = 0;
				uint8_t first_found = 0;
				for (lt_Token* scan = statement_start; scan->type != LT_TOKEN_END; scan++)
				{
					if (scan->line != error_line) continue;
					first_col = scan->col;
					first_found = 1;
					break;
				}
				if (first_found && first_col == error_col) error_line = statement_start->line;
			}

			current = statement_start;
			while (current->type != LT_TOKEN_END && current->type != LT_TOKEN_CLOSEBRACE &&
				current->line <= error_line) current++;
			if (current == statement_start) current++;
			continue;
		}

		vm->error_line = 0;
		vm->error_at_token = 0;

		switch (current->type)
		{
		case LT_TOKEN_CLOSEBRACE:
			if (expects_terminator) { current++; goto end_block; }
			lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected closing brace!");
		case LT_TOKEN_END:
			lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected end of file!");
		case LT_TOKEN_IF: {
			lt_AstNode* if_statement = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_IF);
			current++;
			lt_AstNode* expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			uint8_t was_allow_table_call = p->allow_table_call;
			p->allow_table_call = 0;
			current = _lt_parse_expression(vm, p, current, expr);
			p->allow_table_call = was_allow_table_call;

			if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expeceted open brace to follow if expression!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			lt_Scope* body_scope = _lt_parse_block(vm, p, current, &body, 1, LT_PARSE_SCOPE_LEXICAL, 0);
			current = body_scope->end;

			if_statement->branch.expr = expr;
			if_statement->branch.body = body;
			if_statement->branch.next = 0;
			if_statement->branch.scope = body_scope;

			lt_AstNode* last = 0;
			while (current->type == LT_TOKEN_ELSEIF || current->type == LT_TOKEN_ELSE)
			{

				lt_AstNode* node = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
				if (if_statement->branch.next == 0) if_statement->branch.next = node;
				if (last) last->branch.next = node;

				if (current->type == LT_TOKEN_ELSEIF)
				{
					current++;
					node->type = LT_AST_NODE_ELSEIF;

					lt_AstNode* expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
					uint8_t was_allow_table_call = p->allow_table_call;
					p->allow_table_call = 0;
					current = _lt_parse_expression(vm, p, current, expr);
					p->allow_table_call = was_allow_table_call;

					node->branch.expr = expr;
				}
				else
				{
					current++;
					node->type = LT_AST_NODE_ELSE;
				}

				if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow else expression!");
				current++;

				lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
				lt_Scope* body_scope = _lt_parse_block(vm, p, current, &body, 1, LT_PARSE_SCOPE_LEXICAL, 0);
				current = body_scope->end;

				node->branch.body = body;
				node->branch.scope = body_scope;

				last = node;
			}

			lt_buffer_push(vm, dst, &if_statement);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;

		case LT_TOKEN_FOR: {
			current = _lt_parse_for_statement(vm, p, current, dst, 0);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;

		case LT_TOKEN_WHILE: {
			lt_AstNode* while_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_WHILE);
			current++; // eat while

			lt_AstNode* iter_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			uint8_t was_allow_table_call = p->allow_table_call;
			p->allow_table_call = 0;
			current = _lt_parse_expression(vm, p, current, iter_expr);
			p->allow_table_call = was_allow_table_call;

			while_expr->loop.iterator = iter_expr;

			if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow 'while' header!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			lt_Scope* body_scope = _lt_parse_block(vm, p, current, &body, 1, LT_PARSE_SCOPE_LEXICAL, 0);
			current = body_scope->end;
			while_expr->loop.body = body;
			while_expr->loop.scope = body_scope;

			lt_buffer_push(vm, dst, &while_expr);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;

		case LT_TOKEN_WITH: {
			lt_AstNode* with_stmt = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_WITH);
			current++; // eat with

			with_stmt->with_stmt.expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			uint8_t was_allow_table_call = p->allow_table_call;
			p->allow_table_call = 0;
			current = _lt_parse_expression(vm, p, current, with_stmt->with_stmt.expr);
			p->allow_table_call = was_allow_table_call;

			if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow 'with' expression!");
			with_stmt->with_stmt.receiver = _lt_make_hidden_identifier_token(vm, p, "__with", current);
			current++;

			lt_Token* previous_self = p->self_token;
			lt_Token* with_args[] = { with_stmt->with_stmt.receiver, 0 };
			p->self_token = with_stmt->with_stmt.receiver;
			with_stmt->with_stmt.body = lt_buffer_new(sizeof(lt_AstNode*));
			with_stmt->with_stmt.scope = _lt_parse_block(vm, p, current, &with_stmt->with_stmt.body, 1, LT_PARSE_SCOPE_LEXICAL, with_args);
			p->self_token = previous_self;
			current = with_stmt->with_stmt.scope->end;

			lt_buffer_push(vm, dst, &with_stmt);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;

		case LT_TOKEN_IMPORT: {
			lt_Token* import_token = current++;
			if (current->type != LT_TOKEN_OPENBRACE)
			{
				current = import_token;
				goto parse_expression_statement;
			}

			lt_AstNode* declare = _lt_get_node_of_type(vm, import_token, p, LT_AST_NODE_DECLARE);
			current = _lt_parse_destructure_pattern(vm, p, current, declare);
			if (!_lt_token_is_name(p, current, "from")) lt_parse_error_internal(vm, p->tkn->module, current, "Expected 'from' after import destructuring pattern!");
			current++;

			lt_AstNode* path_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			current = _lt_parse_expression(vm, p, current, path_expr);
			declare->declare.expr = _lt_make_import_call_node(vm, p, import_token, path_expr);

			lt_buffer_push(vm, dst, &declare);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;

		case LT_TOKEN_RETURN: {
			lt_AstNode* ret = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_RETURN);
			ret->ret.expr = 0;
			ret->ret.cleanups = lt_buffer_new(sizeof(uint16_t));
			for (uint32_t i = p->async_cleanup_floor; i < p->async_cleanups.length; ++i)
				lt_buffer_push(vm, &ret->ret.cleanups, lt_buffer_at(&p->async_cleanups, i));
			current++; // eat 'return'

			lt_AstNode* expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			lt_Token* new_current = _lt_parse_expression(vm, p, current, expr);
			if (current != new_current)
			{
				ret->ret.expr = expr;
				current = new_current;
			}

			lt_buffer_push(vm, dst, &ret);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		case LT_TOKEN_BREAK: {
			lt_AstNode* brk = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_BREAK);
			current++; // eat 'break'

			lt_buffer_push(vm, dst, &brk);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		case LT_TOKEN_CLASS: {
			lt_AstNode* klass = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_CLASS);
			current++; // eat class

			if (current->type != LT_TOKEN_IDENTIFIER) lt_parse_error_internal(vm, p->tkn->module, current, "Expected class name to follow 'class'!");
			klass->class_decl.identifier = current++;
			klass->class_decl.members = lt_buffer_new(sizeof(lt_ClassMember));
			lt_make_local_internal(vm, p->current, klass->class_decl.identifier);

			if (current->type == LT_TOKEN_EXTENDS)
			{
				current++;
				if (current->type != LT_TOKEN_IDENTIFIER) lt_parse_error_internal(vm, p->tkn->module, current, "Expected superclass name to follow 'extends'!");
				klass->class_decl.superclass = current++;
			}

			if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow class name!");
			current++;

			while (current->type != LT_TOKEN_CLOSEBRACE)
			{
				if (current->type == LT_TOKEN_END) lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected end of file in class declaration!");

				lt_Visibility visibility = LT_VIS_PUBLIC;
				uint8_t has_visibility = 0;
				uint8_t is_override = 0;
				while (current->type == LT_TOKEN_PUBLIC || current->type == LT_TOKEN_PRIVATE || current->type == LT_TOKEN_OVERRIDE)
				{
					if (current->type == LT_TOKEN_OVERRIDE)
					{
						if (is_override) _lt_parse_soft_error(vm, p, current, "duplicate override modifier!");
						is_override = 1;
						current++;
					}
					else
					{
						if (has_visibility) _lt_parse_soft_error(vm, p, current, "duplicate visibility modifier!");
						has_visibility = 1;
						visibility = current++->type == LT_TOKEN_PRIVATE ? LT_VIS_PRIVATE : LT_VIS_PUBLIC;
					}
				}

				lt_ClassMember member;
				memset(&member, 0, sizeof(member));
				member.visibility = visibility;
				member.is_override = is_override;

				if (current->type == LT_TOKEN_CONSTRUCTOR)
				{
					if (has_visibility) _lt_parse_soft_error(vm, p, current, "constructor cannot be public/private/get/set!");
					if (is_override) _lt_parse_soft_error(vm, p, current, "constructor cannot be override!");
					member.type = LT_CLASS_CONSTRUCTOR;
					member.name = current++;
					member.value = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FN);
					current = _lt_parse_class_function(vm, p, current, member.value, 1, 1, 1);
				}
				else if (current->type == LT_TOKEN_GET || current->type == LT_TOKEN_SET)
				{
					if (is_override && visibility == LT_VIS_PRIVATE) _lt_parse_soft_error(vm, p, current, "private members cannot be override!");
					member.type = current++->type == LT_TOKEN_GET ? LT_CLASS_GETTER : LT_CLASS_SETTER;
					if (current->type != LT_TOKEN_IDENTIFIER) lt_parse_error_internal(vm, p->tkn->module, current, "Expected property name to follow get/set!");
					member.name = current++;
					member.value = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FN);
					current = _lt_parse_class_function(vm, p, current, member.value, member.type == LT_CLASS_GETTER ? 1 : 1, 0, 0);
					uint8_t arity = 0;
					while (member.value->fn.args[arity]) arity++;
					if (member.type == LT_CLASS_GETTER && arity != 1) _lt_parse_soft_error(vm, p, member.name, "getter must have 0 parameters!");
					if (member.type == LT_CLASS_SETTER && arity != 2) _lt_parse_soft_error(vm, p, member.name, "setter must have 1 parameter!");
					if (_lt_class_has_member_kind(&klass->class_decl.members, member.name, member.type, member.visibility))
						_lt_parse_soft_error(vm, p, member.name, member.type == LT_CLASS_GETTER ? "duplicate getter for property!" : "duplicate setter for property!");
					else if (_lt_class_has_conflicting_member(&klass->class_decl.members, member.name, member.type))
						_lt_parse_soft_error(vm, p, member.name, "class members cannot share a name except matching get/set accessors!");
				}
				else if (current->type == LT_TOKEN_IDENTIFIER)
				{
					lt_Token* name = current++;
					member.name = name;
					if (current->type == LT_TOKEN_OPENPAREN)
					{
						member.type = LT_CLASS_METHOD;
						if (is_override && visibility == LT_VIS_PRIVATE) _lt_parse_soft_error(vm, p, member.name, "private members cannot be override!");
						if (_lt_class_has_conflicting_member(&klass->class_decl.members, member.name, member.type))
							_lt_parse_soft_error(vm, p, member.name, "class members cannot share a name except matching get/set accessors!");
						member.value = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FN);
						current = _lt_parse_class_function(vm, p, current, member.value, 1, 0, 0);
					}
					else
					{
						member.type = LT_CLASS_FIELD;
						if (is_override) _lt_parse_soft_error(vm, p, member.name, "fields cannot be override!");
						if (_lt_class_has_conflicting_member(&klass->class_decl.members, member.name, member.type))
							_lt_parse_soft_error(vm, p, member.name, "class members cannot share a name except matching get/set accessors!");
						if (current->type == LT_TOKEN_ASSIGN)
						{
							current++;
							member.value = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
							lt_Token* previous_self = p->self_token;
							p->self_token = _lt_make_identifier_token(vm, p, "this", current);
							current = _lt_parse_expression(vm, p, current, member.value);
							p->self_token = previous_self;
						}
						member.value = _lt_make_field_initializer_fn(vm, p, member.name, member.value);
					}
				}
				else lt_parse_error_internal(vm, p->tkn->module, current, "Expected class member name!");

				lt_buffer_push(vm, &klass->class_decl.members, &member);
			}

			current++; // eat closing brace
			klass->end_token = current - 1;
			lt_buffer_push(vm, dst, &klass);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		case LT_TOKEN_VAR: {
			lt_AstNode* declare = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_DECLARE);
			current = _lt_parse_var_declaration(vm, p, current + 1, declare, 0);
			lt_buffer_push(vm, dst, &declare);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		case LT_TOKEN_GLOBAL: {
			lt_Token* global_token = current++;
			lt_AstNode* declare = _lt_get_node_of_type(vm, global_token, p, LT_AST_NODE_DECLARE);
			if (current->type == LT_TOKEN_IDENTIFIER)
			{
				current = _lt_parse_var_declaration(vm, p, current, declare, 1);
			}
			else if (current->type == LT_TOKEN_FN)
			{
				current = _lt_parse_named_function_declaration(vm, p, current, declare, 1, 0);
			}
			else if (current->type == LT_TOKEN_ASYNC)
			{
				current = _lt_parse_named_function_declaration(vm, p, current, declare, 1, 1);
			}
			else lt_parse_error_internal(vm, p->tkn->module, current, "Expected identifier, 'fn', or 'async fn' to follow 'global'!");

			lt_buffer_push(vm, dst, &declare);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		case LT_TOKEN_FN: {
			lt_Token* fn_token = current++;
			if (current->type != LT_TOKEN_IDENTIFIER)
			{
				current = fn_token;
				goto parse_expression_statement;
			}

			lt_AstNode* declare = _lt_get_node_of_type(vm, fn_token, p, LT_AST_NODE_DECLARE);
			current = _lt_parse_named_function_declaration(vm, p, fn_token, declare, 0, 0);

			lt_buffer_push(vm, dst, &declare);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		case LT_TOKEN_ASYNC: {
			lt_Token* async_token = current++;
			if (current->type == LT_TOKEN_FOR)
			{
				current = _lt_parse_for_statement(vm, p, current, dst, 1);
				REQUIRE_STATEMENT_BOUNDARY();
				break;
			}
			if (current->type != LT_TOKEN_FN || (current + 1)->type != LT_TOKEN_IDENTIFIER)
			{
				current = async_token;
				goto parse_expression_statement;
			}

			lt_AstNode* declare = _lt_get_node_of_type(vm, async_token, p, LT_AST_NODE_DECLARE);
			current = _lt_parse_named_function_declaration(vm, p, async_token, declare, 0, 1);

			lt_buffer_push(vm, dst, &declare);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		default: {
parse_expression_statement:
			lt_Token* expression_start = current;
			lt_AstNode* result = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			current = _lt_parse_expression(vm, p, current, result);

			/* A token that cannot start an expression is consumed by nothing;
			   without this check the block would loop forever. */
			if (current == expression_start)
				lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected token!");

			if (current->type == LT_TOKEN_ASSIGN)
			{
				current++;
				lt_AstNode* expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
				current = _lt_parse_expression(vm, p, current, expr);

				lt_AstNode* lhs = result;

				result = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_ASSIGN);
				result->assign.left = lhs;
				result->assign.right = expr;
			}

			lt_buffer_push(vm, dst, &result);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;
		}

		vm->error_buf = saved_error_buf;
	}

end_block:
	/* A `goto end_block` skips the per-statement restore, and the trap's
	   jmp_buf lives in this frame, so it must be reset before returning. */
	vm->error_buf = block_error_buf;
	p->current->end = current;
	lt_Scope* new_scope = p->current;

	if (scope_type != LT_PARSE_SCOPE_NONE) p->current = p->current->last;

	return new_scope;
}

static lt_Token* _lt_parse_class_function(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* fn, uint8_t add_this, uint8_t allow_auto_assign, uint8_t is_constructor)
{
	lt_Token* paren = current;
	if (current->type != LT_TOKEN_OPENPAREN) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open parenthesis to follow method name!");
	current++;

	uint8_t nargs = 0;
	uint8_t auto_assign[LT_MAX_FUNCTION_PARAMS + 1];
	memset(auto_assign, 0, sizeof(auto_assign));
	/* Anchor the implicit receiver on the parameter list, not on the first
	   parameter, so it does not share that parameter's span. */
	if (add_this) fn->fn.args[nargs++] = _lt_make_identifier_token(vm, p, "this", paren);
	while (current->type == LT_TOKEN_IDENTIFIER || (allow_auto_assign && current->type == LT_TOKEN_AT))
	{
		uint8_t should_auto_assign = 0;
		if (current->type == LT_TOKEN_AT)
		{
			should_auto_assign = 1;
			current++;
			if (current->type != LT_TOKEN_IDENTIFIER && !lt_token_is_keyword_internal(current)) lt_parse_error_internal(vm, p->tkn->module, current, "Expected identifier after '@' constructor parameter!");
		}
		if (nargs >= LT_MAX_FUNCTION_PARAMS) lt_parse_error_internal(vm, p->tkn->module, current, "Too many function parameters!");
		auto_assign[nargs] = should_auto_assign;
		fn->fn.args[nargs++] = lt_token_is_keyword_internal(current) ? _lt_keyword_as_identifier(vm, p, current) : current;
		current++;
		if (current->type == LT_TOKEN_COMMA) current++;
	}

	if (current->type != LT_TOKEN_CLOSEPAREN) lt_parse_error_internal(vm, p->tkn->module, current, "Expected closing parenthesis to follow argument list!");
	current++;

	if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow argument list!");
	current++;

	lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
	uint8_t was_async = p->in_async;
	uint8_t was_constructor = p->in_constructor;
	lt_Token* previous_self = p->self_token;
	uint32_t saved_cleanup_floor = p->async_cleanup_floor;
	p->in_async = fn->fn.is_async;
	p->in_constructor = is_constructor;
	p->async_cleanup_floor = p->async_cleanups.length;
	if (add_this) p->self_token = fn->fn.args[0];
	lt_Scope* fn_scope = _lt_parse_block(vm, p, current, &body, 1, 1, fn->fn.args);
	p->in_async = was_async;
	p->in_constructor = was_constructor;
	p->self_token = previous_self;
	p->async_cleanup_floor = saved_cleanup_floor;
	current = fn_scope->end;

	if (allow_auto_assign)
	{
		lt_Buffer rewritten = lt_buffer_new(sizeof(lt_AstNode*));
		for (uint8_t i = add_this ? 1 : 0; i < nargs; ++i)
		{
			if (!auto_assign[i]) continue;
			lt_AstNode* assign = _lt_make_self_assignment_node(vm, p, fn->fn.args[i], fn->fn.args[0], fn->fn.args[i], fn->fn.args[i]);
			lt_buffer_push(vm, &rewritten, &assign);
		}
		for (uint32_t i = 0; i < body.length; ++i)
		{
			lt_AstNode* node = *(lt_AstNode**)lt_buffer_at(&body, i);
			lt_buffer_push(vm, &rewritten, &node);
		}
		lt_buffer_destroy(vm, &body);
		body = rewritten;
	}

	fn->fn.scope = fn_scope;
	fn->fn.body = body;
	return current;
}

static uint8_t _lt_get_prec(lt_TokenType op)
{
	switch (op)
	{
	case LT_TOKEN_NOT: case LT_TOKEN_NEGATE: case LT_TOKEN_TYPE: case LT_TOKEN_TYPEOF: return 5;
	case LT_TOKEN_MULTIPLY: case LT_TOKEN_DIVIDE: return 4;
	case LT_TOKEN_PLUS: case LT_TOKEN_MINUS: return 3;
	case LT_TOKEN_GT: case LT_TOKEN_GTE: case LT_TOKEN_LT: case LT_TOKEN_LTE: case LT_TOKEN_EQUALS: case LT_TOKEN_NOTEQUALS: return 2;
	case LT_TOKEN_AND: case LT_TOKEN_OR: return 1;
	}

	return 0;
}

static uint8_t _lt_is_unary_operator(lt_TokenType op)
{
	return op == LT_TOKEN_NOT || op == LT_TOKEN_NEGATE || op == LT_TOKEN_TYPE || op == LT_TOKEN_TYPEOF;
}

static uint8_t _lt_should_pop_operator(lt_TokenType top, lt_TokenType current)
{
	uint8_t top_prec = _lt_get_prec(top);
	uint8_t current_prec = _lt_get_prec(current);
	return top_prec > current_prec || (top_prec == current_prec && !_lt_is_unary_operator(current));
}

#define LT_TOKEN_ANY_LITERAL   \
	 LT_TOKEN_NULL_LITERAL:    \
case LT_TOKEN_FALSE_LITERAL:   \
case LT_TOKEN_TRUE_LITERAL:	   \
case LT_TOKEN_NUMBER_LITERAL:  \
case LT_TOKEN_STRING_LITERAL:  \
case LT_TOKEN_FN:              \
case LT_TOKEN_ASYNC

static lt_Token* _lt_parse_table_literal(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode** out)
{
	lt_AstNode* table = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_TABLE);
	table->table.keys = lt_buffer_new(sizeof(lt_AstNode*));
	table->table.values = lt_buffer_new(sizeof(lt_AstNode*));

	current++; // eat brace

	while (current->type != LT_TOKEN_CLOSEBRACE)
	{
		if (current->type == LT_TOKEN_END) lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected end of file in table literal!");

		lt_AstNode* key = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
		lt_Token* key_token = current++;
		key->literal.token = key_token;

		switch (key_token->type)
		{
		case LT_TOKEN_NULL_LITERAL:
		case LT_TOKEN_TRUE_LITERAL:
		case LT_TOKEN_FALSE_LITERAL:
		case LT_TOKEN_NUMBER_LITERAL:
		case LT_TOKEN_STRING_LITERAL:
		case LT_TOKEN_IDENTIFIER:
			break;
		default:
			if (!lt_token_is_keyword_internal(key_token)) lt_parse_error_internal(vm, p->tkn->module, key_token, "Expected identifier, literal, or keyword as table key!");
			break;
		}

		lt_AstNode* value = 0;
		if (current->type == LT_TOKEN_COLON)
		{
			if (lt_token_is_keyword_internal(key_token)) key->literal.token = _lt_keyword_as_identifier(vm, p, key_token);
			current++; // eat colon
			value = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			current = _lt_parse_expression(vm, p, current, value);
		}
		else if (key_token->type == LT_TOKEN_IDENTIFIER)
		{
			value = _lt_get_node_of_type(vm, key_token, p, LT_AST_NODE_IDENTIFIER);
			value->identifier.token = key_token;
		}
		else lt_error(vm, "Expected colon to follow table index!");

		lt_buffer_push(vm, &table->table.keys, &key);
		lt_buffer_push(vm, &table->table.values, &value);
		if (current->type == LT_TOKEN_COMMA) current++;
	}

	*out = table;
	return current + 1; // eat closing brace
}

static lt_Token* _lt_parse_expression(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_AstNode* dst)
{
	uint8_t n_open = 0;
	lt_Token* last = 0;
	lt_Token* current = start;

	lt_Buffer result = lt_buffer_new(sizeof(lt_AstNode*));
	lt_Buffer operator_stack = lt_buffer_new(sizeof(lt_TokenType));
	lt_Buffer operator_tokens = lt_buffer_new(sizeof(lt_Token*));

#define PUSH_EXPR_FROM_OP(op, op_tok) \
	if (op == LT_TOKEN_NOT || op == LT_TOKEN_NEGATE || op == LT_TOKEN_TYPE || op == LT_TOKEN_TYPEOF) \
	{																				\
		lt_AstNode* unaryop = _lt_get_node_of_type(vm, op_tok, p, LT_AST_NODE_UNARYOP);			\
		unaryop->unary_op.type = op;											    \
		lt_buffer_push(vm, &result, &unaryop);											\
	}																				\
	else																			\
	{																				\
		lt_AstNode* binaryop = _lt_get_node_of_type(vm, op_tok, p, LT_AST_NODE_BINARYOP);		\
		binaryop->binary_op.type = op;											    \
		lt_buffer_push(vm, &result, &binaryop);											\
	}

#define BREAK_ON_EXPR_BOUNDRY       \
	if (last) switch (last->type)   \
	{								\
		case LT_TOKEN_IDENTIFIER:	\
		case LT_TOKEN_CLOSEBRACE:	\
		case LT_TOKEN_CLOSEBRACKET:	\
		case LT_TOKEN_CLOSEPAREN:	\
		case LT_TOKEN_ANY_LITERAL:	\
			goto expr_end;			\
	}

	while (current->type != LT_TOKEN_END)
	{
		switch (current->type)
		{
		case LT_TOKEN_IDENTIFIER: {
			BREAK_ON_EXPR_BOUNDRY

			lt_AstNode* ident = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_IDENTIFIER);
			ident->identifier.token = NEXT();

			if (lt_find_local_internal(vm, p->current, ident->identifier.token) == NOT_FOUND) {} // ERROR!

			lt_buffer_push(vm, &result, &ident);
			} break;
		case LT_TOKEN_AT: {
			BREAK_ON_EXPR_BOUNDRY
			lt_Token* loc = current;
			NEXT();
			if (lt_token_is_keyword_internal(current))
			{
				lt_AstNode* index = _lt_make_self_index_node(vm, p, loc, _lt_keyword_as_identifier(vm, p, current));
				last = index->index.idx->literal.token;
				current++;
				lt_buffer_push(vm, &result, &index);
			}
			else
			{
				if (current->type != LT_TOKEN_IDENTIFIER) lt_parse_error_internal(vm, p->tkn->module, current, "Expected identifier after '@'!");
				lt_AstNode* index = _lt_make_self_index_node(vm, p, loc, NEXT());
				lt_buffer_push(vm, &result, &index);
			}
			} break;
		case LT_TOKEN_SUPER: {
			BREAK_ON_EXPR_BOUNDRY
			lt_Token* loc = current;
			NEXT();
			if (!p->self_token) lt_parse_error_internal(vm, p->tkn->module, loc, "'super' is only valid inside class methods!");
			lt_AstNode* super = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_SUPER);
			if (current->type == LT_TOKEN_PERIOD)
			{
				current++;
				if (lt_token_is_keyword_internal(current)) super->super_expr.method = _lt_keyword_as_identifier(vm, p, current++);
				else if (current->type != LT_TOKEN_IDENTIFIER) lt_parse_error_internal(vm, p->tkn->module, current, "Expected method name after 'super.'!");
				else super->super_expr.method = current++;
			}
			else
			{
				if (!p->in_constructor)
					lt_parse_error_internal(vm, p->tkn->module, loc, "super(...) is only valid inside constructors!");
				if (current->type != LT_TOKEN_OPENPAREN)
					lt_parse_error_internal(vm, p->tkn->module, current, "Expected '(' after 'super'!");
			}
			lt_buffer_push(vm, &result, &super);
			last = super->super_expr.method ? super->super_expr.method : loc;
		} break;
		case LT_TOKEN_OPENBRACKET: {
			uint8_t is_index = last != 0;
			if (last) switch(last->type)
			{
			case LT_TOKEN_CLOSEBRACE:
			case LT_TOKEN_CLOSEBRACKET:
			case LT_TOKEN_CLOSEPAREN:
			case LT_TOKEN_IDENTIFIER:
				is_index = 1;
			}

			if (is_index)
			{
				NEXT(); // eat bracket
				lt_AstNode* idx_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
				current = _lt_parse_expression(vm, p, current, idx_expr);
				if (current->type != LT_TOKEN_CLOSEBRACKET) lt_parse_error_internal(vm, p->tkn->module, current, "Expected closing bracket to follow index expression!");
				NEXT();

				lt_AstNode* source = *(void**)lt_buffer_last(&result); lt_buffer_pop(&result);
				lt_AstNode* index = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_INDEX);
				index->index.source = source;
				index->index.idx = idx_expr;
				lt_buffer_push(vm, &result, &index);
			}
			else
			{
				// array literal
				NEXT(); // eat bracket
				lt_AstNode* arr_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_ARRAY);
				arr_expr->array.values = lt_buffer_new(sizeof(lt_AstNode*));

				while (current->type != LT_TOKEN_CLOSEBRACKET)
				{
					lt_AstNode* value = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
					current = _lt_parse_expression(vm, p, current, value);
					lt_buffer_push(vm, &arr_expr->array.values, &value);

					if (current->type == LT_TOKEN_COMMA) current++;
				}

				NEXT();
				lt_buffer_push(vm, &result, &arr_expr);
			}
			} break;
		case LT_TOKEN_PERIOD: {
			uint8_t allowed = 0;
			if (last) switch (last->type)
			{
			case LT_TOKEN_CLOSEBRACE:
			case LT_TOKEN_CLOSEBRACKET:
			case LT_TOKEN_CLOSEPAREN:
			case LT_TOKEN_IDENTIFIER:
				allowed = 1;
			}

			if (!allowed) goto expr_end;

			lt_Token* period = current;
			NEXT(); // eat period
			lt_AstNode* idx_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
			if (current->line != period->line && _lt_starts_statement(current->type))
			{
				_lt_parse_soft_error(vm, p, period, "Expected identifier to follow '.' operator!");
				goto expr_end;
			}
			if (lt_token_is_keyword_internal(current))
			{
				idx_expr->literal.token = _lt_keyword_as_identifier(vm, p, current);
				last = idx_expr->literal.token;
				current++;
			}
			else if (current->type != LT_TOKEN_IDENTIFIER)
			{
				/* A dangling `obj.` ends the line; do not consume the next
				   statement's first token as the missing member. */
				if (current->line != period->line)
				{
					_lt_parse_soft_error(vm, p, period, "Expected identifier to follow '.' operator!");
					goto expr_end;
				}
				lt_parse_error_internal(vm, p->tkn->module, current, "Expected identifier to follow '.' operator!");
			}
			else idx_expr->literal.token = NEXT();

			lt_AstNode* source = *(void**)lt_buffer_last(&result); lt_buffer_pop(&result);
			lt_AstNode* index = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_INDEX);
			index->index.source = source;
			index->index.idx = idx_expr;
			lt_buffer_push(vm, &result, &index);
		} break;

		case LT_TOKEN_COLON: {
			uint8_t allowed = 0;
			if (last) switch (last->type)
			{
			case LT_TOKEN_CLOSEBRACE:
			case LT_TOKEN_CLOSEBRACKET:
			case LT_TOKEN_CLOSEPAREN:
			case LT_TOKEN_IDENTIFIER:
				allowed = 1;
			}

			if (!allowed) goto expr_end;

			lt_Token* colon = current;
			NEXT(); // eat colon
			lt_AstNode* idx_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
			if (current->line != colon->line && _lt_starts_statement(current->type))
			{
				_lt_parse_soft_error(vm, p, colon, "Expected identifier to follow ':' operator!");
				goto expr_end;
			}
			if (lt_token_is_keyword_internal(current))
			{
				idx_expr->literal.token = _lt_keyword_as_identifier(vm, p, current);
				last = idx_expr->literal.token;
				current++;
			}
			else if (current->type != LT_TOKEN_IDENTIFIER)
			{
				if (current->line != colon->line)
				{
					_lt_parse_soft_error(vm, p, colon, "Expected identifier to follow ':' operator!");
					goto expr_end;
				}
				lt_parse_error_internal(vm, p->tkn->module, current, "Expected identifier to follow ':' operator!");
			}
			else idx_expr->literal.token = NEXT();

			lt_AstNode* source = *(void**)lt_buffer_last(&result); lt_buffer_pop(&result);
			lt_AstNode* index = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_INDEX);
			index->index.source = source;
			index->index.idx = idx_expr;

			lt_AstNode* call = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_CALL);
			uint8_t nargs = 0;
			call->call.args[nargs++] = source;
			call->call.callee = index;

			if (current->type == LT_TOKEN_OPENPAREN)
			{
				NEXT(); // eat open paren
				while (current->type != LT_TOKEN_CLOSEPAREN)
				{
					if (current->type == LT_TOKEN_END) lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected end of file in expression. (Unclosed method call?)");
					if (current->type == LT_TOKEN_COMMA) NEXT();

					lt_AstNode* arg = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
					current = _lt_parse_expression(vm, p, current, arg);
					if (nargs >= LT_MAX_CALL_ARGS) lt_parse_error_internal(vm, p->tkn->module, current, "Too many call arguments!");
					call->call.args[nargs++] = arg;
				}

				NEXT(); // eat close paren
			}
			else if (p->allow_table_call && current->type == LT_TOKEN_OPENBRACE)
			{
				lt_AstNode* table = 0;
				current = _lt_parse_table_literal(vm, p, current, &table);
				last = current - 1;
				call->call.args[nargs++] = table;
			}
			else lt_parse_error_internal(vm, p->tkn->module, current, "Expected call arguments to follow ':' method access!");

			lt_buffer_push(vm, &result, &call);
		} break;

		case LT_TOKEN_NUMBER_LITERAL: case LT_TOKEN_NULL_LITERAL: case LT_TOKEN_TRUE_LITERAL: case LT_TOKEN_FALSE_LITERAL: case LT_TOKEN_STRING_LITERAL: {
			BREAK_ON_EXPR_BOUNDRY

			lt_AstNode* lit = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
			lit->literal.token = NEXT();
			lt_buffer_push(vm, &result, &lit);
		} break;

		case LT_TOKEN_PLUS: case LT_TOKEN_MINUS:
		case LT_TOKEN_MULTIPLY: case LT_TOKEN_DIVIDE:
		case LT_TOKEN_EQUALS: case LT_TOKEN_NOTEQUALS:
		case LT_TOKEN_GT: case LT_TOKEN_GTE: case LT_TOKEN_LT: case LT_TOKEN_LTE:
		case LT_TOKEN_AND: case LT_TOKEN_OR: case LT_TOKEN_NOT: case LT_TOKEN_TYPE: case LT_TOKEN_TYPEOF: {
			lt_TokenType optype = current->type;

			if (optype == LT_TOKEN_MINUS)
			{
				optype = LT_TOKEN_NEGATE;

				if (last) switch (last->type)
				{
				case LT_TOKEN_ANY_LITERAL:
				case LT_TOKEN_IDENTIFIER:
				case LT_TOKEN_CLOSEPAREN:
				case LT_TOKEN_CLOSEBRACKET:
					optype = LT_TOKEN_MINUS;
				}
			}

			while (operator_stack.length > 0)
			{
				if (_lt_should_pop_operator(*(lt_TokenType*)lt_buffer_last(&operator_stack), optype))
				{
					lt_TokenType shunted = *(lt_TokenType*)lt_buffer_last(&operator_stack);
					lt_buffer_pop(&operator_stack);
					lt_Token* shunted_tok = *(lt_Token**)lt_buffer_last(&operator_tokens);
					lt_buffer_pop(&operator_tokens);

					PUSH_EXPR_FROM_OP(shunted, shunted_tok);
				}
				else break;
			}

			lt_buffer_push(vm, &operator_stack, &optype);
			lt_buffer_push(vm, &operator_tokens, &current);
			NEXT();
		} break;

		case LT_TOKEN_OPENPAREN: {
			uint8_t is_call = 0;
			if (last) switch (last->type)
			{
			case LT_TOKEN_CLOSEPAREN:
			case LT_TOKEN_CLOSEBRACE:
			case LT_TOKEN_IDENTIFIER:
			case LT_TOKEN_SUPER:
			case LT_TOKEN_CLOSEBRACKET:
				is_call = 1;
				break;
			}

			if (is_call)
			{
				NEXT();
				lt_AstNode* callee = *(lt_AstNode**)lt_buffer_last(&result); lt_buffer_pop(&result);

				lt_AstNode* call = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_CALL);
				uint8_t nargs = 0;

				while (current->type != LT_TOKEN_CLOSEPAREN)
				{
					if (current->type == LT_TOKEN_END) lt_parse_error_internal(vm, p->tkn->module, current, "Unexpected end of file in expression. (Unclosed parenthesis?)");
					if (current->type == LT_TOKEN_COMMA) NEXT();

					lt_AstNode* arg = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
					current = _lt_parse_expression(vm, p, current, arg);
					if (nargs >= LT_MAX_CALL_ARGS) lt_parse_error_internal(vm, p->tkn->module, current, "Too many call arguments!");
					call->call.args[nargs++] = arg;
				}

				call->call.callee = callee;
				lt_buffer_push(vm, &result, &call);
				NEXT();
			}
			else
			{
				n_open++;
				lt_buffer_push(vm, &operator_stack, &current->type);
				lt_buffer_push(vm, &operator_tokens, &current);
				NEXT();
			}
		} break;

		case LT_TOKEN_CLOSEPAREN: {
			if (n_open == 0) goto expr_end;
			NEXT();
			while (operator_stack.length > 0)
			{
				lt_TokenType back = *(lt_TokenType*)lt_buffer_last(&operator_stack);

				if (back == LT_TOKEN_OPENPAREN) break;

				lt_buffer_pop(&operator_stack);
				lt_Token* back_tok = *(lt_Token**)lt_buffer_last(&operator_tokens);
				lt_buffer_pop(&operator_tokens);
				PUSH_EXPR_FROM_OP(back, back_tok);
			}

			if (operator_stack.length == 0) lt_parse_error_internal(vm, p->tkn->module, current, "Malformed expression!");
			else
			{
				lt_buffer_pop(&operator_stack);
				lt_buffer_pop(&operator_tokens);
				n_open--;
			}
		} break;

		case LT_TOKEN_OPENBRACE: {
			uint8_t is_table_call = p->allow_table_call && _lt_is_postfix_source(last);
			if (!is_table_call) BREAK_ON_EXPR_BOUNDRY

			lt_AstNode* table = 0;
			current = _lt_parse_table_literal(vm, p, current, &table);
			last = current - 1;
			if (is_table_call)
			{
				lt_AstNode* callee = *(lt_AstNode**)lt_buffer_last(&result);
				lt_buffer_pop(&result);

				lt_AstNode* call = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_CALL);
				call->call.callee = callee;
				call->call.args[0] = table;
				lt_buffer_push(vm, &result, &call);
			}
			else lt_buffer_push(vm, &result, &table);
		} break;

		case LT_TOKEN_AWAIT: {
			BREAK_ON_EXPR_BOUNDRY
			if (!p->in_async)
			{
				char sprint_buf[128];
				sprintf_s(sprint_buf, 128, "%s|%d:%d: 'await' is only valid inside async functions!", p->tkn->module, current->line, current->col);
				if (vm->error) vm->error(vm, sprint_buf);
				p->had_error = 1;
				while (current->type != LT_TOKEN_END) current++;
				goto expr_end;
			}

			lt_AstNode* await = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_AWAIT);
			NEXT();
			await->await.expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			current = _lt_parse_expression(vm, p, current, await->await.expr);
			last = current - 1;
			lt_buffer_push(vm, &result, &await);
		} break;

		case LT_TOKEN_IMPORT: {
			BREAK_ON_EXPR_BOUNDRY
			lt_Token* import_token = current;
			NEXT();
			lt_AstNode* path_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			current = _lt_parse_expression(vm, p, current, path_expr);
			lt_AstNode* call = _lt_make_import_call_node(vm, p, import_token, path_expr);
			lt_buffer_push(vm, &result, &call);
			goto expr_end;
		} break;

		case LT_TOKEN_ASYNC:
		case LT_TOKEN_FN: {
			BREAK_ON_EXPR_BOUNDRY

			uint8_t is_async = current->type == LT_TOKEN_ASYNC;
			if (is_async)
			{
				NEXT();
				if (current->type != LT_TOKEN_FN) lt_parse_error_internal(vm, p->tkn->module, current, "Expected 'fn' to follow 'async'!");
			}

			lt_AstNode* func = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FN);
			func->fn.is_async = is_async;
			NEXT();

			if (current->type != LT_TOKEN_OPENPAREN) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open parenthesis to follow 'fn'!");
			current++;

			uint8_t nargs = 0;
			while (current->type == LT_TOKEN_IDENTIFIER)
			{
				if (nargs >= LT_MAX_FUNCTION_PARAMS) lt_parse_error_internal(vm, p->tkn->module, current, "Too many function parameters!");
				func->fn.args[nargs++] = current++;
				if (current->type == LT_TOKEN_COMMA) current++;
			}

			if (current->type != LT_TOKEN_CLOSEPAREN) lt_parse_error_internal(vm, p->tkn->module, current, "Expecetd closing parenthesis to follow argument list!");
			current++;

			if (current->type != LT_TOKEN_OPENBRACE) lt_parse_error_internal(vm, p->tkn->module, current, "Expected open brace to follow argument list!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			uint8_t was_async = p->in_async;
			uint8_t was_constructor = p->in_constructor;
			lt_Token* previous_self = p->self_token;
			uint32_t saved_cleanup_floor = p->async_cleanup_floor;
			lt_Token* this_token = _lt_make_identifier_token(vm, p, "this", current);
			p->in_async = is_async;
			p->in_constructor = 0;
			p->async_cleanup_floor = p->async_cleanups.length;
			if (nargs > 0 && _lt_tokens_equal(func->fn.args[0], this_token)) p->self_token = func->fn.args[0];
			lt_Scope* fn_scope = _lt_parse_block(vm, p, current, &body, 1, 1, func->fn.args);
			p->in_async = was_async;
			p->in_constructor = was_constructor;
			p->self_token = previous_self;
			p->async_cleanup_floor = saved_cleanup_floor;
			current = fn_scope->end;

			func->fn.scope = fn_scope;
			func->fn.body = body;
			lt_buffer_push(vm, &result, &func);
		} break;

		default: {
			if (last) goto expr_end;
			else lt_parse_error_internal(vm, p->tkn->module, current, "Malformed expression!");
		}
		}
	}

expr_end:
	while (operator_stack.length > 0)
	{
		lt_TokenType back = *(lt_TokenType*)lt_buffer_last(&operator_stack);
		lt_buffer_pop(&operator_stack);
		lt_Token* back_tok = *(lt_Token**)lt_buffer_last(&operator_tokens);
		lt_buffer_pop(&operator_tokens);
		PUSH_EXPR_FROM_OP(back, back_tok);
	}

	/* A malformed expression that starts with an operator (for example a lone
	   `/`) produces a postfix stream with more operators than operands. Check
	   that before folding, otherwise the value stack is read below its start. */
	int32_t operands = 0;
	for (uint32_t i = 0; i < result.length; i++)
	{
		lt_AstNode* node = *(lt_AstNode**)lt_buffer_at(&result, i);
		if (node->type == LT_AST_NODE_BINARYOP)
		{
			if (operands < 2) lt_syntax_error_internal(vm, p->tkn->module, node->span, node->loc.line, node->loc.col, "Malformed expression!");
			operands -= 1;
		}
		else if (node->type == LT_AST_NODE_UNARYOP)
		{
			if (operands < 1) lt_syntax_error_internal(vm, p->tkn->module, node->span, node->loc.line, node->loc.col, "Malformed expression!");
		}
		else operands += 1;
	}

	lt_Buffer value_stack = lt_buffer_new(sizeof(lt_AstNode*));

	for (uint32_t i = 0; i < result.length; i++)
	{
		lt_AstNode* current = *(lt_AstNode**)lt_buffer_at(&result, i);
		if (current->type == LT_AST_NODE_BINARYOP)
		{
			lt_AstNode* right = *(lt_AstNode**)lt_buffer_last(&value_stack); lt_buffer_pop(&value_stack);
			lt_AstNode* left = *(lt_AstNode**)lt_buffer_last(&value_stack); lt_buffer_pop(&value_stack);

			switch (current->binary_op.type)
			{
			case LT_TOKEN_LT:
			case LT_TOKEN_LTE:
				current->binary_op.type -= 2;
				current->binary_op.left = right;
				current->binary_op.right = left;
				break;
			default:
				current->binary_op.left = left;
				current->binary_op.right = right;
				break;
			}
		}
		else if (current->type == LT_AST_NODE_UNARYOP)
		{
			lt_AstNode* right = *(lt_AstNode**)lt_buffer_last(&value_stack); lt_buffer_pop(&value_stack);
			current->unary_op.expr = right;
		}

		lt_buffer_push(vm, &value_stack, &current);
	}

	if (value_stack.length > 0)
	{
		lt_AstNode* source = *(lt_AstNode**)lt_buffer_at(&value_stack, 0);
		memcpy(dst, source, sizeof(lt_AstNode));
		source->type = LT_AST_NODE_EMPTY;
	}

	lt_buffer_destroy(vm, &result);
	lt_buffer_destroy(vm, &operator_stack);
	lt_buffer_destroy(vm, &operator_tokens);
	lt_buffer_destroy(vm, &value_stack);

	return current;
}

static uint32_t _lt_token_span_end(lt_Token* token)
{
	return token ? token->span.end : 0;
}

/* A block's scope ends on the token after its closing brace, so the brace
   itself is the previous token. */
static uint32_t _lt_scope_span_end(lt_Parser* p, lt_Scope* scope)
{
	if (!scope || !scope->end) return 0;

	lt_Token* tokens = (lt_Token*)p->tkn->token_buffer.data;
	if (scope->end <= tokens) return 0;
	return (scope->end - 1)->span.end;
}

static void _lt_raise(uint32_t* end, uint32_t candidate)
{
	if (candidate > *end) *end = candidate;
}

static uint32_t _lt_compute_node_end(lt_VM* vm, lt_Parser* p, lt_AstNode* node);

static uint32_t _lt_compute_body_end(lt_VM* vm, lt_Parser* p, lt_Buffer* body)
{
	uint32_t end = 0;
	for (uint32_t i = 0; i < body->length; i++)
		_lt_raise(&end, _lt_compute_node_end(vm, p, *(lt_AstNode**)lt_buffer_at(body, i)));
	return end;
}

/* Extends each node's span to cover everything it contains, so tooling gets a
   declaration range rather than just the token that introduced it. */
static uint32_t _lt_compute_node_end(lt_VM* vm, lt_Parser* p, lt_AstNode* node)
{
	if (!node) return 0;

	uint32_t end = node->span.end;
	_lt_raise(&end, _lt_token_span_end(node->end_token));

	switch (node->type)
	{
	case LT_AST_NODE_CHUNK:
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->chunk.body));
		_lt_raise(&end, _lt_scope_span_end(p, node->chunk.scope));
		break;
	case LT_AST_NODE_TABLE:
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->table.keys));
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->table.values));
		break;
	case LT_AST_NODE_ARRAY:
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->array.values));
		break;
	case LT_AST_NODE_INDEX:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->index.source));
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->index.idx));
		break;
	case LT_AST_NODE_BINARYOP:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->binary_op.left));
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->binary_op.right));
		break;
	case LT_AST_NODE_UNARYOP:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->unary_op.expr));
		break;
	case LT_AST_NODE_DECLARE:
		_lt_raise(&end, _lt_token_span_end(node->declare.identifier));
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->declare.expr));
		for (uint32_t i = 0; i < node->declare.entries.length; i++)
		{
			lt_DestructureEntry* entry = lt_buffer_at(&node->declare.entries, i);
			_lt_raise(&end, _lt_token_span_end(entry->key));
			_lt_raise(&end, _lt_token_span_end(entry->local));
		}
		break;
	case LT_AST_NODE_CLASS:
		_lt_raise(&end, _lt_token_span_end(node->class_decl.identifier));
		_lt_raise(&end, _lt_token_span_end(node->class_decl.superclass));
		for (uint32_t i = 0; i < node->class_decl.members.length; i++)
		{
			lt_ClassMember* member = lt_buffer_at(&node->class_decl.members, i);
			_lt_raise(&end, _lt_token_span_end(member->name));
			_lt_raise(&end, _lt_compute_node_end(vm, p, member->value));
		}
		break;
	case LT_AST_NODE_ASSIGN:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->assign.left));
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->assign.right));
		break;
	case LT_AST_NODE_FN:
		for (uint8_t i = 0; i <= LT_MAX_FUNCTION_PARAMS; i++)
			_lt_raise(&end, _lt_token_span_end(node->fn.args[i]));
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->fn.body));
		_lt_raise(&end, _lt_scope_span_end(p, node->fn.scope));
		break;
	case LT_AST_NODE_CALL:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->call.callee));
		for (uint8_t i = 0; i <= LT_MAX_CALL_ARGS + 1; i++)
			_lt_raise(&end, _lt_compute_node_end(vm, p, node->call.args[i]));
		break;
	case LT_AST_NODE_AWAIT:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->await.expr));
		break;
	case LT_AST_NODE_RETURN:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->ret.expr));
		break;
	case LT_AST_NODE_IF:
	case LT_AST_NODE_ELSEIF:
	case LT_AST_NODE_ELSE:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->branch.expr));
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->branch.body));
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->branch.next));
		break;
	case LT_AST_NODE_FOR:
	case LT_AST_NODE_WHILE:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->loop.iterator));
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->loop.body));
		break;
	case LT_AST_NODE_WITH:
		_lt_raise(&end, _lt_compute_node_end(vm, p, node->with_stmt.expr));
		_lt_raise(&end, _lt_compute_body_end(vm, p, &node->with_stmt.body));
		_lt_raise(&end, _lt_scope_span_end(p, node->with_stmt.scope));
		break;
	case LT_AST_NODE_SUPER:
		_lt_raise(&end, _lt_token_span_end(node->super_expr.method));
		break;
	default:
		break;
	}

	node->span.end = end;
	return end;
}

lt_Parser lt_parse(lt_VM* vm, lt_Tokenizer* tkn)
{
	lt_Parser p;
	memset(&p, 0, sizeof(lt_Parser));
	p.is_valid = 0;
	p.tkn = tkn;
	p.ast_nodes = lt_buffer_new(sizeof(lt_AstNode*));
	p.generated_tokens = lt_buffer_new(sizeof(lt_Token*));
	p.scopes = lt_buffer_new(sizeof(lt_Scope*));
	p.async_cleanups = lt_buffer_new(sizeof(uint16_t));
	p.async_cleanup_floor = 0;

	void* saved_error_buf = vm->error_buf;
	jmp_buf error_buf;
	vm->error_buf = &error_buf;

	if (!setjmp(error_buf))
	{
		p.current = 0;
		p.in_async = 0;
		p.allow_table_call = 1;
		p.had_error = 0;
		vm->error_line = 0;
		p.root = _lt_get_node_of_type(vm, (lt_Token*)tkn->token_buffer.data, &p, LT_AST_NODE_CHUNK);
		p.root->chunk.body = lt_buffer_new(sizeof(lt_AstNode*));

		lt_Scope* file_scope = _lt_parse_block(vm, &p, tkn->token_buffer.data, &p.root->chunk.body, 0, 1, 0);

		p.root->chunk.scope = file_scope;
		_lt_compute_node_end(vm, &p, p.root);
		p.is_valid = !p.had_error;
	}

	vm->error_buf = saved_error_buf;
	return p;
}


void lt_free_scope(lt_VM* vm, lt_Scope* scope)
{
	lt_buffer_destroy(vm, &scope->locals);
	lt_buffer_destroy(vm, &scope->local_slots);
	lt_buffer_destroy(vm, &scope->frame_slots);
	lt_buffer_destroy(vm, &scope->upvals);
	lt_buffer_destroy(vm, &scope->captured);
}

void lt_free_parser(lt_VM* vm, lt_Parser* p)
{
	for (uint32_t i = 0; i < p->ast_nodes.length; i++)
	{
		lt_AstNode* entry = *(lt_AstNode**)lt_buffer_at(&p->ast_nodes, i);

		switch (entry->type)
		{
		case LT_AST_NODE_CHUNK:
			lt_buffer_destroy(vm, &entry->chunk.body);
			break;
		case LT_AST_NODE_CLASS:
			lt_buffer_destroy(vm, &entry->class_decl.members);
			break;
		case LT_AST_NODE_DECLARE:
			if (entry->declare.destructure != LT_DESTRUCT_NONE) lt_buffer_destroy(vm, &entry->declare.entries);
			break;
		case LT_AST_NODE_TABLE: lt_buffer_destroy(vm, &entry->table.keys); lt_buffer_destroy(vm, &entry->table.values); break;
		case LT_AST_NODE_ARRAY: lt_buffer_destroy(vm, &entry->array.values); break;
		case LT_AST_NODE_RETURN: lt_buffer_destroy(vm, &entry->ret.cleanups); break;
		case LT_AST_NODE_IF: case LT_AST_NODE_ELSEIF: case LT_AST_NODE_ELSE: lt_buffer_destroy(vm, &entry->branch.body); break;
		case LT_AST_NODE_WITH:
			lt_buffer_destroy(vm, &entry->with_stmt.body);
			break;
		}

		vm->free(entry);
	}

	/* Scopes are owned by the parser, not by the nodes that reference them, so
	   partially parsed and abandoned scopes are released too. */
	for (uint32_t i = 0; i < p->scopes.length; i++)
	{
		lt_Scope* scope = *(lt_Scope**)lt_buffer_at(&p->scopes, i);
		lt_free_scope(vm, scope);
		vm->free(scope);
	}

	for (uint32_t i = 0; i < p->generated_tokens.length; i++)
		vm->free(*(lt_Token**)lt_buffer_at(&p->generated_tokens, i));
	lt_buffer_destroy(vm, &p->generated_tokens);
	lt_buffer_destroy(vm, &p->scopes);
	lt_buffer_destroy(vm, &p->async_cleanups);
	lt_buffer_destroy(vm, &p->ast_nodes);
}
