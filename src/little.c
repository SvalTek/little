#include "little_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

static lt_Value LT_NULL = LT_VALUE_NULL;

/* Microsoft provides these bounds-checked functions; provide the small subset
 * Little uses on other C runtimes. */
#if !defined(_WIN32)
#include <stdarg.h>

static int sprintf_s(char *restrict buffer, size_t bufsz, const char *restrict format, ...)
{
	va_list args;
    va_start(args, format);
	int r = vsnprintf(buffer, bufsz, format, args);
	va_end(args);
	return r;
}

static int strncpy_s(char *restrict dest, size_t destsz, const char *restrict src, size_t count)
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

typedef struct {
	uint64_t hash;
	char* string;
	lt_Value value;
	uint32_t refcount;
	uint32_t len;
} lt_StringDedupEntry;

typedef enum {
	LT_OP_NOP,

	LT_OP_PUSH, LT_OP_POP, LT_OP_DUP,

	LT_OP_PUSHS, LT_OP_PUSHC, LT_OP_PUSHN, LT_OP_PUSHT, LT_OP_PUSHF,

	LT_OP_ADD, LT_OP_SUB, LT_OP_MUL, LT_OP_DIV, LT_OP_NEG,
	LT_OP_EQ, LT_OP_NEQ, LT_OP_GT, LT_OP_GTE,
	LT_OP_AND, LT_OP_OR, LT_OP_NOT, LT_OP_TYPE, LT_OP_TYPEOF,

	LT_OP_LOAD, LT_OP_STORE, LT_OP_LOADCELL, LT_OP_STORECELL,
	LT_OP_LOADUP, LT_OP_STOREUP, LT_OP_LOADUPCELL, LT_OP_CAPTURE,
	LT_OP_LOADB,
	LT_OP_AWAIT,

	LT_OP_CLOSE, LT_OP_CALL, LT_OP_CALLM, LT_OP_FIXRET, LT_OP_PACKRET, LT_OP_SUPERC, LT_OP_SUPERM,

	LT_OP_MAKET, LT_OP_MAKEA, LT_OP_MAKEC, LT_OP_SETSUPER, LT_OP_SETT, LT_OP_SETC, LT_OP_GETT, LT_OP_GETD, LT_OP_GETG, LT_OP_SETG,

	LT_OP_JMP, LT_OP_JMPC, LT_OP_JMPN,

	LT_OP_RET, LT_OP_RETM,
} lt_OpCode;

typedef struct {
	uint16_t op;
	int16_t arg;
} lt_Op;

static void _lt_table_init(lt_Table* table);
static void _lt_table_destroy(lt_VM* vm, lt_Table* table);
static void _lt_table_mark(lt_VM* vm, lt_Table* table);
static lt_Value _lt_table_get_raw(lt_Table* table, lt_Value key);
static lt_Value _lt_table_set_raw(lt_VM* vm, lt_Table* table, lt_Value key, lt_Value val);
static lt_Value _lt_class_call(lt_VM* vm, lt_Value class_value, uint8_t argc);
static lt_Value _lt_instance_get(lt_VM* vm, lt_Value instance_value, lt_Value key);
static void _lt_instance_set(lt_VM* vm, lt_Value instance_value, lt_Value key, lt_Value value);

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

static void _lt_tokenize_error(lt_VM* vm, const char* module, uint16_t line, uint16_t col, const char* message)
{
	char sprint_buf[128];
	sprintf_s(sprint_buf, 128, "%s|%d:%d: %s", module, line, col, message);
	lt_error(vm, sprint_buf);
}

static void _lt_parse_error(lt_VM* vm, const char* module, lt_Token* t, const char* message)
{
	char sprint_buf[128];
	sprintf_s(sprint_buf, 128, "%s|%d:%d: %s", module, t->line, t->col, message);
	lt_error(vm, sprint_buf);
}

static lt_DebugInfo* _lt_get_debuginfo(lt_Object* obj)
{
	switch (obj->type)
	{
	case LT_OBJECT_CHUNK: return obj->chunk.debug;
	case LT_OBJECT_FN: return obj->fn.debug;
	case LT_OBJECT_CLOSURE: return LT_GET_OBJECT(obj->closure.function)->fn.debug;
	}

	return 0;
}

static lt_DebugLoc _lt_get_location(lt_DebugInfo* info, uint32_t pc)
{
	if (info)
	{
		return *(lt_DebugLoc*)lt_buffer_at(&info->locations, pc);
	}

	return (lt_DebugLoc){ 0, 0 };
}

void lt_runtime_error(lt_VM* vm, const char* message)
{
	char sprint_buf[1024];

	lt_Frame* topmost = &vm->callstack[vm->depth - 1];
	lt_DebugInfo* info = _lt_get_debuginfo(topmost->callee);
	lt_DebugLoc loc = _lt_get_location(info, topmost->pc);

	const char* name = "<unknown>";
	if (info) name = info->module_name;

	int written = snprintf(sprint_buf, 1024, "%s|%d:%d: %s\ntraceback:", name, loc.line, loc.col, message);
	uint32_t len = written > 0 ? (uint32_t)written : 0;
	if (len >= 1024) len = 1023;
	for (int32_t i = (int32_t)vm->depth - 1; i >= 0; --i)
	{
		lt_Frame* frame = &vm->callstack[i];
		lt_DebugInfo* info = _lt_get_debuginfo(frame->callee);
		lt_DebugLoc loc = _lt_get_location(info, 0);

		const char* name = "<unknown>";
		if (info) name = info->module_name;
		written = snprintf(sprint_buf + len, 1024 - len, "\n(%s|%d:%d)", name, loc.line, loc.col);
		if (written > 0) len += (uint32_t)written;
		if (len >= 1024) { len = 1023; break; }
	}

	lt_error(vm, sprint_buf);
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

static void _lt_reference_string(lt_VM* vm, lt_Value value)
{
	uint16_t bucket = (uint16_t)((value & 0xFFFFFF000000) >> 24);
	uint32_t index = value & 0xFFFFFF;
	((lt_StringDedupEntry*)lt_buffer_at(vm->strings + bucket, index))->refcount++;
}

static uint8_t faststrcmp(const char* a, uint64_t a_len, const char* b, uint64_t b_len)
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

static void _lt_table_init(lt_Table* table)
{
	memset(table, 0, sizeof(lt_Table));
}

static void _lt_table_destroy(lt_VM* vm, lt_Table* table)
{
	for (uint8_t i = 0; i < 16; ++i)
		lt_buffer_destroy(vm, table->buckets + i);
}

static void _lt_table_mark(lt_VM* vm, lt_Table* table)
{
	for (uint16_t i = 0; i < 16; ++i)
	{
		lt_Buffer* bucket = table->buckets + i;
		for (uint32_t j = 0; j < bucket->length; ++j)
		{
			lt_TablePair* pair = lt_buffer_at(bucket, j);
			lt_sweep_v(vm, pair->key);
			lt_sweep_v(vm, pair->value);
		}
	}
}

static lt_TablePair* _lt_table_index_raw(lt_VM* vm, lt_Table* table, lt_Value key, uint8_t alloc)
{
	uint8_t bucket_idx = HASH_VALUE(key);
	lt_Buffer* bucket = table->buckets + bucket_idx;
	if (alloc && bucket->element_size == 0) *bucket = lt_buffer_new(sizeof(lt_TablePair));

	for (uint32_t i = 0; i < bucket->length; i++)
	{
		lt_TablePair* pair = lt_buffer_at(bucket, i);
		if (lt_equals(pair->key, key)) return pair;
	}

	return 0;
}

static lt_Value _lt_table_get_raw(lt_Table* table, lt_Value key)
{
	lt_TablePair* pair = _lt_table_index_raw(0, table, key, 0);
	return pair ? pair->value : LT_VALUE_NULL;
}

static lt_Value _lt_table_set_raw(lt_VM* vm, lt_Table* table, lt_Value key, lt_Value val)
{
	lt_TablePair* pair = _lt_table_index_raw(vm, table, key, 1);
	if (pair)
	{
		pair->value = val;
		return val;
	}

	lt_Buffer* bucket = table->buckets + HASH_VALUE(key);
	lt_TablePair newpair = { key, val };
	lt_buffer_push(vm, bucket, &newpair);
	return val;
}

lt_Tokenizer lt_tokenize(lt_VM* vm, const char* source, const char* mod_name)
{
	lt_Tokenizer t;
	t.module = mod_name;
	t.is_valid = 0;
	t.source = source;
	t.token_buffer = lt_buffer_new(sizeof(lt_Token));
	t.identifier_buffer = lt_buffer_new(sizeof(lt_Identifier));
	t.literal_buffer = lt_buffer_new(sizeof(lt_Literal));

	void* saved_error_buf = vm->error_buf;
	jmp_buf error_buf;
	vm->error_buf = &error_buf;

	if (!setjmp(error_buf))
	{
		const char* current = source;
		uint16_t line = 1, col = 0;

#define PUSH_TOKEN(new_type) { \
	lt_Token _t; _t.type = new_type; _t.line = line; _t.col = col++; _t.idx = 0; \
	lt_buffer_push(vm, &t.token_buffer, &_t); current++; found = 1;\
};

		while (*current)
		{
			uint8_t found = 0;
			switch (*current)
			{
			case ' ': case '\t': { col++; current++; } found = 1; break;
			case '\n': { col = 0; line++; current++; } found = 1; break;
			case '\r': { current++; } found = 1; break;
			case ';': { while (*current++ != '\n'); col = 1; line++; found = 1; } break;
			case '.': PUSH_TOKEN(LT_TOKEN_PERIOD)		   break;
			case ',': PUSH_TOKEN(LT_TOKEN_COMMA)		   break;
			case ':': PUSH_TOKEN(LT_TOKEN_COLON)		   break;
			case '@': PUSH_TOKEN(LT_TOKEN_AT)			   break;
			case '(': PUSH_TOKEN(LT_TOKEN_OPENPAREN)	   break;
			case ')': PUSH_TOKEN(LT_TOKEN_CLOSEPAREN)	   break;
			case '[': PUSH_TOKEN(LT_TOKEN_OPENBRACKET)	   break;
			case ']': PUSH_TOKEN(LT_TOKEN_CLOSEBRACKET)	   break;
			case '{': PUSH_TOKEN(LT_TOKEN_OPENBRACE)	   break;
			case '}': PUSH_TOKEN(LT_TOKEN_CLOSEBRACE)	   break;
			case '+': PUSH_TOKEN(LT_TOKEN_PLUS)			   break;
			case '-': PUSH_TOKEN(LT_TOKEN_MINUS)		   break;
			case '*': PUSH_TOKEN(LT_TOKEN_MULTIPLY)		   break;
			case '/': PUSH_TOKEN(LT_TOKEN_DIVIDE)		   break;
			case '=': PUSH_TOKEN(LT_TOKEN_ASSIGN)		   break;
			}

			if (!found)
			{
				if (*current == '>')
				{
					if (*(current + 1) == '=')
					{
						current++;
						PUSH_TOKEN(LT_TOKEN_GTE);
					}
					else PUSH_TOKEN(LT_TOKEN_GT);
				}
				else if (*current == '<')
				{
					if (*(current + 1) == '=')
					{
						current++;
						PUSH_TOKEN(LT_TOKEN_LTE);
					}
					else PUSH_TOKEN(LT_TOKEN_LT);
				}
				else if (*current == '"')
				{
					const char* start = ++current;
					uint32_t raw_length = 0;
					while (*current != '"')
					{
						if (*current == 0) break;
						if (*current == '\\' && *(current + 1) != 0)
						{
							current += 2;
							raw_length += 2;
							continue;
						}
						if (*current == '\n') { col = 0; line++; }
						current++;
						raw_length++;
					}

					const char* end = current;
					if (*current == '"') current++;

					lt_Literal newlit;
					newlit.type = LT_TOKEN_STRING_LITERAL;
					newlit.string = vm->alloc(raw_length + 1);

					uint32_t length = 0;
					for (const char* src = start; src < end; ++src)
					{
						if (*src == '\\' && src + 1 < end)
						{
							src++;
							switch (*src)
							{
							case 'n': newlit.string[length++] = '\n'; break;
							case 'r': newlit.string[length++] = '\r'; break;
							case 't': newlit.string[length++] = '\t'; break;
							case '"': newlit.string[length++] = '"'; break;
							case '\\': newlit.string[length++] = '\\'; break;
							default:
								newlit.string[length++] = '\\';
								newlit.string[length++] = *src;
								break;
							}
						}
						else newlit.string[length++] = *src;
					}
					newlit.string[length] = 0;

					lt_buffer_push(vm, &t.literal_buffer, &newlit);

					lt_Token tok;
					tok.type = LT_TOKEN_STRING_LITERAL;
					tok.line = line;
					tok.col = col; col += raw_length;
					tok.idx = t.literal_buffer.length - 1;
					lt_buffer_push(vm, &t.token_buffer, &tok);
				}
				else if (isalnum(*current) && !isalpha(*current))
				{
					const char* start = current;
					uint16_t start_col = col;
					uint8_t has_decimal = 0;
					double number = 0;
					uint32_t length = 0;

					if (*current == '0' && (*(current + 1) == 'x' || *(current + 1) == 'X'))
					{
						current += 2;
						uint8_t has_digits = isxdigit(*current) != 0;
						if (!has_digits) _lt_tokenize_error(vm, t.module, line, col, "Expected hex digits after 0x!");
						while (isxdigit(*current)) current++;
						if (*current == '.' || isalnum(*current) || *current == '_')
						{
							_lt_tokenize_error(vm, t.module, line, col, "Invalid character in hex number literal!");
							while (*current == '.' || isalnum(*current) || *current == '_') current++;
						}

						length = (uint32_t)(current - start);
						char* end = 0;
						unsigned long long parsed = strtoull(start + 2, &end, 16);
						if (has_digits && end != current) _lt_tokenize_error(vm, t.module, line, col, "Failed to parse hex number!");
						number = has_digits ? (double)parsed : 0;
					}
					else
					{
						while ((isalnum(*current) && !isalpha(*current)) || *current == '.')
						{
							if (*current == '.')
							{
								if (has_decimal) _lt_tokenize_error(vm, t.module, line, col, "Can't have multiple decimals in number literal!");
								has_decimal = 1;
							}

							current++;
						}

						length = (uint32_t)(current - start);
						char* end = 0;
						number = strtod(start, &end);

						if (end != current) _lt_tokenize_error(vm, t.module, line, col, "Failed to parse number!");
					}

					lt_Literal newlit;
					newlit.type = LT_TOKEN_NUMBER_LITERAL;
					newlit.number = number;

					lt_buffer_push(vm, &t.literal_buffer, &newlit);

					lt_Token tok;
					tok.type = LT_TOKEN_NUMBER_LITERAL;
					tok.line = line;
					tok.col = start_col; col += length;
					tok.idx = t.literal_buffer.length - 1;
					lt_buffer_push(vm, &t.token_buffer, &tok);
				}
				else if (isalpha(*current) || *current == '_')
				{
					const char* start = current;
					uint8_t search = 1;
					while (search)
					{
						current++;
						if (!isalnum(*current) && *current != '_')
						{
							search = 0;
						}
					}

					uint16_t length = (uint16_t)(current - start);

#define PUSH_STR_TOKEN(name, new_type) \
	if(faststrcmp(name, sizeof(name) - 1, start, length)) { \
		lt_Token _t; _t.type = new_type; _t.line = line; _t.col = col; col += length; _t.idx = 0; \
		lt_buffer_push(vm, &t.token_buffer, &_t); found = 1; }

				PUSH_STR_TOKEN("fn", LT_TOKEN_FN)
				else PUSH_STR_TOKEN("var", LT_TOKEN_VAR)
				else PUSH_STR_TOKEN("global", LT_TOKEN_GLOBAL)
				else PUSH_STR_TOKEN("if", LT_TOKEN_IF)
				else PUSH_STR_TOKEN("else", LT_TOKEN_ELSE)
				else PUSH_STR_TOKEN("elseif", LT_TOKEN_ELSEIF)
				else PUSH_STR_TOKEN("for", LT_TOKEN_FOR)
				else PUSH_STR_TOKEN("in", LT_TOKEN_IN)
				else PUSH_STR_TOKEN("while", LT_TOKEN_WHILE)
				else PUSH_STR_TOKEN("with", LT_TOKEN_WITH)
				else PUSH_STR_TOKEN("import", LT_TOKEN_IMPORT)
				else PUSH_STR_TOKEN("from", LT_TOKEN_FROM)
				else PUSH_STR_TOKEN("break", LT_TOKEN_BREAK)
				else PUSH_STR_TOKEN("return", LT_TOKEN_RETURN)
				else PUSH_STR_TOKEN("async", LT_TOKEN_ASYNC)
				else PUSH_STR_TOKEN("await", LT_TOKEN_AWAIT)
				else PUSH_STR_TOKEN("class", LT_TOKEN_CLASS)
				else PUSH_STR_TOKEN("extends", LT_TOKEN_EXTENDS)
				else PUSH_STR_TOKEN("override", LT_TOKEN_OVERRIDE)
				else PUSH_STR_TOKEN("super", LT_TOKEN_SUPER)
				else PUSH_STR_TOKEN("public", LT_TOKEN_PUBLIC)
				else PUSH_STR_TOKEN("private", LT_TOKEN_PRIVATE)
				else PUSH_STR_TOKEN("constructor", LT_TOKEN_CONSTRUCTOR)
				else PUSH_STR_TOKEN("get", LT_TOKEN_GET)
				else PUSH_STR_TOKEN("set", LT_TOKEN_SET)
				else PUSH_STR_TOKEN("is", LT_TOKEN_EQUALS)
				else PUSH_STR_TOKEN("isnt", LT_TOKEN_NOTEQUALS)
		else PUSH_STR_TOKEN("and", LT_TOKEN_AND)
		else PUSH_STR_TOKEN("or", LT_TOKEN_OR)
		else PUSH_STR_TOKEN("not", LT_TOKEN_NOT)
		else PUSH_STR_TOKEN("type", LT_TOKEN_TYPE)
		else PUSH_STR_TOKEN("typeof", LT_TOKEN_TYPEOF)
		else PUSH_STR_TOKEN("true", LT_TOKEN_TRUE_LITERAL)
				else PUSH_STR_TOKEN("false", LT_TOKEN_FALSE_LITERAL)
				else PUSH_STR_TOKEN("null", LT_TOKEN_NULL_LITERAL)

				if (!found)
				{
					for (uint32_t i = 0; i < t.identifier_buffer.length; i++)
					{
						lt_Identifier* id = lt_buffer_at(&t.identifier_buffer, i);
						if (faststrcmp(start, length, id->name, strlen(id->name)))
						{
							found = 1;
							id->num_references++;

							lt_Token tok;
							tok.type = LT_TOKEN_IDENTIFIER;
							tok.line = line;
							tok.col = col; col += length;
							tok.idx = i;
							lt_buffer_push(vm, &t.token_buffer, &tok);
							break;
						}
					}

					if (!found)
					{
						lt_Identifier newid;
						newid.num_references = 1;
						newid.name = vm->alloc(length + 1);
						strncpy_s(newid.name, length + 1, start, length);
						newid.name[length] = 0;

						lt_buffer_push(vm, &t.identifier_buffer, &newid);

						lt_Token tok;
						tok.type = LT_TOKEN_IDENTIFIER;
						tok.line = line;
						tok.col = col; col += length;
						tok.idx = t.identifier_buffer.length - 1;
						lt_buffer_push(vm, &t.token_buffer, &tok);
					}
				}
				}
				else _lt_tokenize_error(vm, t.module, line, col, "Unrecognized token!");
			}
		}

		lt_Token tok;
		tok.type = LT_TOKEN_END;
		tok.line = line;
		tok.col = col;
		lt_buffer_push(vm, &t.token_buffer, &tok);

		t.is_valid = 1;
	}

	vm->error_buf = saved_error_buf;
	return t;
}

lt_AstNode* _lt_get_node_of_type(lt_VM* vm, lt_Token* current, lt_Parser* p, lt_AstNodeType type)
{
	lt_AstNode* new_node = vm->alloc(sizeof(lt_AstNode));
	memset(new_node, 0, sizeof(lt_AstNode));
	new_node->type = type;
	new_node->loc.line = current->line;
	new_node->loc.col = current->col;

	lt_buffer_push(vm, &p->ast_nodes, &new_node);
	return *(void**)lt_buffer_last(&p->ast_nodes);
}

uint32_t _lt_tokens_equal(lt_Token* a, lt_Token* b)
{
	return (a->type == LT_TOKEN_IDENTIFIER && b->type == LT_TOKEN_IDENTIFIER && a->idx == b->idx);
}

uint16_t _lt_make_local(lt_VM* vm, lt_Scope* scope, lt_Token* t)
{
	lt_Scope* current = scope;
	
	for (uint32_t i = 0; i < current->locals.length; ++i)
	{
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&current->locals, i), t)) return i;
	}

	if (current->locals.length >= LT_MAX_LOCALS) lt_error(vm, "Too many local variables!");
	lt_buffer_push(vm, &current->locals, t);
	return current->locals.length - 1;
}

static void _lt_mark_captured_local(lt_VM* vm, lt_Scope* scope, lt_Token* t)
{
	for (uint32_t i = 0; i < scope->captured.length; ++i)
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&scope->captured, i), t)) return;
	lt_buffer_push(vm, &scope->captured, t);
}

static uint8_t _lt_is_captured_local(lt_Scope* scope, uint32_t idx)
{
	if (idx >= scope->locals.length) return 0;
	lt_Token* local = lt_buffer_at(&scope->locals, idx);
	for (uint32_t i = 0; i < scope->captured.length; ++i)
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&scope->captured, i), local)) return 1;
	return 0;
}


#define UPVAL_BIT 0x07000000
#define NOT_FOUND ((uint32_t)-1)

uint32_t _lt_find_local(lt_VM* vm, lt_Scope* scope, lt_Token* t)
{
	lt_Scope* current = scope;
	
	for (uint32_t i = 0; i < current->locals.length; ++i)
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&current->locals, i), t)) return i;

	for (uint32_t i = 0; i < current->upvals.length; ++i)
		if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&current->upvals, i), t)) return i | UPVAL_BIT;

	lt_Scope* test = current->last;
	while (test)
	{
		lt_Token* found_local = 0;
		for (uint32_t i = 0; i < test->locals.length; ++i)
		{
			lt_Token* local = lt_buffer_at(&test->locals, i);
			if (_lt_tokens_equal(local, t)) { found_local = local; break; }
		}

		if(!found_local)
			for (uint32_t i = 0; i < test->upvals.length; ++i)
			{
				if (_lt_tokens_equal((lt_Token*)lt_buffer_at(&test->upvals, i), t)) { found_local = t; break; }
			}

		if (found_local)
		{
			for (uint32_t i = 0; i < test->locals.length; ++i)
			{
				lt_Token* local = lt_buffer_at(&test->locals, i);
				if (_lt_tokens_equal(local, t))
				{
					_lt_mark_captured_local(vm, test, local);
					break;
				}
			}
			lt_buffer_push(vm, &current->upvals, t);
			return (current->upvals.length - 1) | UPVAL_BIT;
		}

		test = test->last;
	}

	return NOT_FOUND;
}

lt_Token* _lt_parse_expression(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_AstNode* dst);
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
	return tok;
}

static lt_Token* _lt_make_number_token(lt_VM* vm, lt_Parser* p, double number, lt_Token* loc)
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
		if (_lt_find_local(vm, p->current, this_token) != NOT_FOUND) self = this_token;
	}
	if (!self) _lt_parse_error(vm, p->tkn->module, loc, "'@' is only valid where 'this' is available!");

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
	fn_scope->start = loc;
	fn_scope->end = loc;
	fn_scope->locals = lt_buffer_new(sizeof(lt_Token));
	fn_scope->upvals = lt_buffer_new(sizeof(lt_Token));
	fn_scope->captured = lt_buffer_new(sizeof(lt_Token));
	_lt_make_local(vm, fn_scope, fn->fn.args[0]);
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
		if (!is_global) _lt_make_local(vm, p->current, declare->declare.identifier);
	}
	else if (!is_global && (current->type == LT_TOKEN_OPENBRACKET || current->type == LT_TOKEN_OPENBRACE))
	{
		current = _lt_parse_destructure_pattern(vm, p, current, declare);
	}
	else _lt_parse_error(vm, p->tkn->module, current, is_global ? "Expected identifier to follow 'global'!" : "Expected identifier or destructuring pattern to follow 'var'!");

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

static lt_Token* _lt_parse_named_function_declaration(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* declare, uint8_t is_global, uint8_t is_async)
{
	lt_Token* loc = current;
	if (is_async)
	{
		if (current->type != LT_TOKEN_ASYNC) _lt_parse_error(vm, p->tkn->module, current, "Expected 'async' in async function declaration!");
		current++;
		if (current->type != LT_TOKEN_FN) _lt_parse_error(vm, p->tkn->module, current, "Expected 'fn' to follow 'async'!");
	}
	else if (current->type != LT_TOKEN_FN)
		_lt_parse_error(vm, p->tkn->module, current, "Expected 'fn' in function declaration!");

	current++;
	if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, is_global ? "Expected function name to follow 'global fn'!" : "Expected function name to follow 'fn'!");
	declare->declare.identifier = current++;
	if (!is_global) _lt_make_local(vm, p->current, declare->declare.identifier);

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
			_lt_make_local(vm, p->current, entry.local);
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
			_lt_make_local(vm, p->current, entry.local);
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

lt_Scope* _lt_parse_block(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_Buffer* dst, uint8_t expects_terminator, uint8_t makes_scope, lt_Token** argnames)
{
	if (makes_scope)
	{
		lt_Scope* new_scope = vm->alloc(sizeof(lt_Scope));
		new_scope->last = p->current;
		p->current = new_scope;

		new_scope->start = start;
		new_scope->end = start;

		new_scope->locals = lt_buffer_new(sizeof(lt_Token));
		new_scope->upvals = lt_buffer_new(sizeof(lt_Token));
		new_scope->captured = lt_buffer_new(sizeof(lt_Token));

		if (argnames) while (*argnames) _lt_make_local(vm, p->current, *argnames++);
	}

	lt_Token* current = start;

#define PEEK() (current + 1)
#define NEXT() (last = current, current++)
#define REQUIRE_STATEMENT_BOUNDARY() \
	if (current->type != LT_TOKEN_END && current->type != LT_TOKEN_CLOSEBRACE && current > start && (current - 1)->line == current->line) \
		_lt_parse_error(vm, p->tkn->module, current, "Expected newline between statements!")

	while (current->type != LT_TOKEN_END)
	{
		switch (current->type)
		{
		case LT_TOKEN_CLOSEBRACE:
			if (expects_terminator) { current++; goto end_block; }
			_lt_parse_error(vm, p->tkn->module, current, "Unexpected closing brace!");
		case LT_TOKEN_END:
			_lt_parse_error(vm, p->tkn->module, current, "Unexpected end of file!");
		case LT_TOKEN_IF: {
			lt_AstNode* if_statement = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_IF);
			current++;
			lt_AstNode* expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			uint8_t was_allow_table_call = p->allow_table_call;
			p->allow_table_call = 0;
			current = _lt_parse_expression(vm, p, current, expr);
			p->allow_table_call = was_allow_table_call;

			if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expeceted open brace to follow if expression!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			_lt_parse_block(vm, p, current, &body, 1, 0, 0);
			current = p->current->end;

			if_statement->branch.expr = expr;
			if_statement->branch.body = body;
			if_statement->branch.next = 0;

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

				if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow else expression!");
				current++;

				lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
				_lt_parse_block(vm, p, current, &body, 1, 0, 0);
				current = p->current->end;

				node->branch.body = body;

				last = node;
			}

			lt_buffer_push(vm, dst, &if_statement);
			REQUIRE_STATEMENT_BOUNDARY();
		} break;

		case LT_TOKEN_FOR: {
			lt_AstNode* for_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FOR);
			current++; // eat for

			lt_Token* ident = current++;
			uint16_t iteridx = _lt_make_local(vm, p->current, ident);

			if (current->type != LT_TOKEN_IN) _lt_parse_error(vm, p->tkn->module, current, "Expected 'in' to follow 'for' iterator!");
			current++;

			lt_AstNode* iter_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			uint8_t was_allow_table_call = p->allow_table_call;
			p->allow_table_call = 0;
			current = _lt_parse_expression(vm, p, current, iter_expr);
			p->allow_table_call = was_allow_table_call;

			for_expr->loop.identifier = iteridx;
			
			const char* FOR_ITER_NAME = "__iter";
			uint16_t len = (uint16_t)strlen(FOR_ITER_NAME);

			lt_Identifier newid;
			newid.num_references = 1;
			newid.name = vm->alloc(len + 1);
			strncpy_s(newid.name, len + 1, FOR_ITER_NAME, len);
			newid.name[len] = 0;

			lt_buffer_push(vm, &p->tkn->identifier_buffer, &newid);

			lt_Token tok;
			tok.type = LT_TOKEN_IDENTIFIER;
			tok.line = ident->line;
			tok.col = ident->col;
			tok.idx = p->tkn->identifier_buffer.length - 1;

			for_expr->loop.closureidx = _lt_make_local(vm, p->current, &tok);
			for_expr->loop.iterator = iter_expr;

			if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow 'for' header!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			_lt_parse_block(vm, p, current, &body, 1, 0, 0);
			current = p->current->end;
			for_expr->loop.body = body;

			lt_buffer_push(vm, dst, &for_expr);
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

			if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow 'while' header!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			_lt_parse_block(vm, p, current, &body, 1, 0, 0);
			current = p->current->end;
			while_expr->loop.body = body;

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

			if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow 'with' expression!");
			with_stmt->with_stmt.receiver = _lt_make_hidden_identifier_token(vm, p, "__with", current);
			_lt_make_local(vm, p->current, with_stmt->with_stmt.receiver);
			current++;

			lt_Token* previous_self = p->self_token;
			p->self_token = with_stmt->with_stmt.receiver;
			with_stmt->with_stmt.body = lt_buffer_new(sizeof(lt_AstNode*));
			_lt_parse_block(vm, p, current, &with_stmt->with_stmt.body, 1, 0, 0);
			p->self_token = previous_self;
			current = p->current->end;

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
			if (current->type != LT_TOKEN_FROM) _lt_parse_error(vm, p->tkn->module, current, "Expected 'from' after import destructuring pattern!");
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

			if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected class name to follow 'class'!");
			klass->class_decl.identifier = current++;
			klass->class_decl.members = lt_buffer_new(sizeof(lt_ClassMember));
			_lt_make_local(vm, p->current, klass->class_decl.identifier);

			if (current->type == LT_TOKEN_EXTENDS)
			{
				current++;
				if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected superclass name to follow 'extends'!");
				klass->class_decl.superclass = current++;
			}

			if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow class name!");
			current++;

			while (current->type != LT_TOKEN_CLOSEBRACE)
			{
				if (current->type == LT_TOKEN_END) _lt_parse_error(vm, p->tkn->module, current, "Unexpected end of file in class declaration!");

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
					if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected property name to follow get/set!");
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
				else _lt_parse_error(vm, p->tkn->module, current, "Expected class member name!");

				lt_buffer_push(vm, &klass->class_decl.members, &member);
			}

			current++; // eat closing brace
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
			else _lt_parse_error(vm, p->tkn->module, current, "Expected identifier, 'fn', or 'async fn' to follow 'global'!");

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
			lt_AstNode* result = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
			current = _lt_parse_expression(vm, p, current, result);

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
	}
	
end_block:
	p->current->end = current;
	lt_Scope* new_scope = p->current;

	if(makes_scope) p->current = p->current->last;

	return new_scope;
}

static lt_Token* _lt_parse_class_function(lt_VM* vm, lt_Parser* p, lt_Token* current, lt_AstNode* fn, uint8_t add_this, uint8_t allow_auto_assign, uint8_t is_constructor)
{
	if (current->type != LT_TOKEN_OPENPAREN) _lt_parse_error(vm, p->tkn->module, current, "Expected open parenthesis to follow method name!");
	current++;

	uint8_t nargs = 0;
	uint8_t auto_assign[LT_MAX_FUNCTION_PARAMS + 1];
	memset(auto_assign, 0, sizeof(auto_assign));
	if (add_this) fn->fn.args[nargs++] = _lt_make_identifier_token(vm, p, "this", current);
	while (current->type == LT_TOKEN_IDENTIFIER || (allow_auto_assign && current->type == LT_TOKEN_AT))
	{
		uint8_t should_auto_assign = 0;
		if (current->type == LT_TOKEN_AT)
		{
			should_auto_assign = 1;
			current++;
			if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected identifier after '@' constructor parameter!");
		}
		if (nargs >= LT_MAX_FUNCTION_PARAMS) _lt_parse_error(vm, p->tkn->module, current, "Too many function parameters!");
		auto_assign[nargs] = should_auto_assign;
		fn->fn.args[nargs++] = current++;
		if (current->type == LT_TOKEN_COMMA) current++;
	}

	if (current->type != LT_TOKEN_CLOSEPAREN) _lt_parse_error(vm, p->tkn->module, current, "Expected closing parenthesis to follow argument list!");
	current++;

	if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow argument list!");
	current++;

	lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
	uint8_t was_async = p->in_async;
	uint8_t was_constructor = p->in_constructor;
	lt_Token* previous_self = p->self_token;
	p->in_async = fn->fn.is_async;
	p->in_constructor = is_constructor;
	if (add_this) p->self_token = fn->fn.args[0];
	lt_Scope* fn_scope = _lt_parse_block(vm, p, current, &body, 1, 1, fn->fn.args);
	p->in_async = was_async;
	p->in_constructor = was_constructor;
	p->self_token = previous_self;
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

uint8_t _lt_get_prec(lt_TokenType op)
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
		if (current->type == LT_TOKEN_END) _lt_parse_error(vm, p->tkn->module, current, "Unexpected end of file in table literal!");

		lt_AstNode* key = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
		lt_Token* key_token = current++;
		key->literal.token = key_token;

		lt_AstNode* value = 0;
		if (current->type == LT_TOKEN_COLON)
		{
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

lt_Token* _lt_parse_expression(lt_VM* vm, lt_Parser* p, lt_Token* start, lt_AstNode* dst)
{
	uint8_t n_open = 0;
	lt_Token* last = 0;
	lt_Token* current = start;

	lt_Buffer result = lt_buffer_new(sizeof(lt_AstNode*));
	lt_Buffer operator_stack = lt_buffer_new(sizeof(lt_TokenType));

#define PUSH_EXPR_FROM_OP(op) \
	if (op == LT_TOKEN_NOT || op == LT_TOKEN_NEGATE || op == LT_TOKEN_TYPE || op == LT_TOKEN_TYPEOF) \
	{																				\
		lt_AstNode* unaryop = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_UNARYOP);			\
		unaryop->unary_op.type = op;											    \
		lt_buffer_push(vm, &result, &unaryop);											\
	}																				\
	else																			\
	{																				\
		lt_AstNode* binaryop = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_BINARYOP);		\
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

			if (_lt_find_local(vm, p->current, ident->identifier.token) == NOT_FOUND) {} // ERROR!

			lt_buffer_push(vm, &result, &ident);
			} break;
		case LT_TOKEN_AT: {
			BREAK_ON_EXPR_BOUNDRY
			lt_Token* loc = current;
			NEXT();
			if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected identifier after '@'!");
			lt_AstNode* index = _lt_make_self_index_node(vm, p, loc, NEXT());
			lt_buffer_push(vm, &result, &index);
			} break;
		case LT_TOKEN_SUPER: {
			BREAK_ON_EXPR_BOUNDRY
			lt_Token* loc = current;
			NEXT();
			if (!p->self_token) _lt_parse_error(vm, p->tkn->module, loc, "'super' is only valid inside class methods!");
			lt_AstNode* super = _lt_get_node_of_type(vm, loc, p, LT_AST_NODE_SUPER);
			if (current->type == LT_TOKEN_PERIOD)
			{
				current++;
				if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected method name after 'super.'!");
				super->super_expr.method = current++;
			}
			else
			{
				if (!p->in_constructor)
					_lt_parse_error(vm, p->tkn->module, loc, "super(...) is only valid inside constructors!");
				if (current->type != LT_TOKEN_OPENPAREN)
					_lt_parse_error(vm, p->tkn->module, current, "Expected '(' after 'super'!");
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
				if (current->type != LT_TOKEN_CLOSEBRACKET) _lt_parse_error(vm, p->tkn->module, current, "Expected closing bracket to follow index expression!");
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

			NEXT(); // eat period
			lt_AstNode* idx_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
			if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected identifier to follow '.' operator!");
			idx_expr->literal.token = NEXT();

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

			NEXT(); // eat colon
			lt_AstNode* idx_expr = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_LITERAL);
			if (current->type != LT_TOKEN_IDENTIFIER) _lt_parse_error(vm, p->tkn->module, current, "Expected identifier to follow ':' operator!");
			idx_expr->literal.token = NEXT();

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
					if (current->type == LT_TOKEN_END) _lt_parse_error(vm, p->tkn->module, current, "Unexpected end of file in expression. (Unclosed method call?)");
					if (current->type == LT_TOKEN_COMMA) NEXT();

					lt_AstNode* arg = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
					current = _lt_parse_expression(vm, p, current, arg);
					if (nargs >= LT_MAX_CALL_ARGS) _lt_parse_error(vm, p->tkn->module, current, "Too many call arguments!");
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
			else _lt_parse_error(vm, p->tkn->module, current, "Expected call arguments to follow ':' method access!");

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

					PUSH_EXPR_FROM_OP(shunted);
				}
				else break;
			}

			lt_buffer_push(vm, &operator_stack, &optype);
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
					if (current->type == LT_TOKEN_END) _lt_parse_error(vm, p->tkn->module, current, "Unexpected end of file in expression. (Unclosed parenthesis?)");
					if (current->type == LT_TOKEN_COMMA) NEXT();

					lt_AstNode* arg = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_EMPTY);
					current = _lt_parse_expression(vm, p, current, arg);
					if (nargs >= LT_MAX_CALL_ARGS) _lt_parse_error(vm, p->tkn->module, current, "Too many call arguments!");
					call->call.args[nargs++] = arg;
				}

				call->call.callee = callee;
				lt_buffer_push(vm, &result, &call);
				NEXT();
			}
			else
			{
				n_open++;
				lt_buffer_push(vm, &operator_stack, &current->type); NEXT();
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
				PUSH_EXPR_FROM_OP(back);
			}

			if (operator_stack.length == 0) _lt_parse_error(vm, p->tkn->module, current, "Malformed expression!");
			else
			{
				lt_buffer_pop(&operator_stack);
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
				if (current->type != LT_TOKEN_FN) _lt_parse_error(vm, p->tkn->module, current, "Expected 'fn' to follow 'async'!");
			}

			lt_AstNode* func = _lt_get_node_of_type(vm, current, p, LT_AST_NODE_FN);
			func->fn.is_async = is_async;
			NEXT();

			if (current->type != LT_TOKEN_OPENPAREN) _lt_parse_error(vm, p->tkn->module, current, "Expected open parenthesis to follow 'fn'!");
			current++;

			uint8_t nargs = 0;
			while (current->type == LT_TOKEN_IDENTIFIER)
			{
				if (nargs >= LT_MAX_FUNCTION_PARAMS) _lt_parse_error(vm, p->tkn->module, current, "Too many function parameters!");
				func->fn.args[nargs++] = current++;
				if (current->type == LT_TOKEN_COMMA) current++;
			}

			if (current->type != LT_TOKEN_CLOSEPAREN) _lt_parse_error(vm, p->tkn->module, current, "Expecetd closing parenthesis to follow argument list!");
			current++;

			if (current->type != LT_TOKEN_OPENBRACE) _lt_parse_error(vm, p->tkn->module, current, "Expected open brace to follow argument list!");
			current++;

			lt_Buffer body = lt_buffer_new(sizeof(lt_AstNode*));
			uint8_t was_async = p->in_async;
			uint8_t was_constructor = p->in_constructor;
			lt_Token* previous_self = p->self_token;
			lt_Token* this_token = _lt_make_identifier_token(vm, p, "this", current);
			p->in_async = is_async;
			p->in_constructor = 0;
			if (nargs > 0 && _lt_tokens_equal(func->fn.args[0], this_token)) p->self_token = func->fn.args[0];
			lt_Scope* fn_scope = _lt_parse_block(vm, p, current, &body, 1, 1, func->fn.args);
			p->in_async = was_async;
			p->in_constructor = was_constructor;
			p->self_token = previous_self;
			current = fn_scope->end;

			func->fn.scope = fn_scope;
			func->fn.body = body;
			lt_buffer_push(vm, &result, &func);
		} break;

		default: {
			if (last) goto expr_end;
			else _lt_parse_error(vm, p->tkn->module, current, "Malformed expression!");
		}
		}
	}

expr_end:
	while (operator_stack.length > 0)
	{
		lt_TokenType back = *(lt_TokenType*)lt_buffer_last(&operator_stack);
		lt_buffer_pop(&operator_stack);
		PUSH_EXPR_FROM_OP(back);
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
	lt_buffer_destroy(vm, &value_stack);

	return current;
}

lt_Parser lt_parse(lt_VM* vm, lt_Tokenizer* tkn)
{
	lt_Parser p;
	memset(&p, 0, sizeof(lt_Parser));
	p.is_valid = 0;
	p.tkn = tkn;
	p.ast_nodes = lt_buffer_new(sizeof(lt_AstNode*));

	void* saved_error_buf = vm->error_buf;
	jmp_buf error_buf;
	vm->error_buf = &error_buf;

	if (!setjmp(error_buf))
	{
		p.current = 0;
		p.in_async = 0;
		p.allow_table_call = 1;
		p.had_error = 0;
		p.root = _lt_get_node_of_type(vm, (lt_Token*)tkn->token_buffer.data, &p, LT_AST_NODE_CHUNK);
		p.root->chunk.body = lt_buffer_new(sizeof(lt_AstNode*));

		lt_Scope* file_scope = _lt_parse_block(vm, &p, tkn->token_buffer.data, &p.root->chunk.body, 0, 1, 0);

		p.root->chunk.scope = file_scope;
		p.is_valid = !p.had_error;
	}

	vm->error_buf = saved_error_buf;
	return p;
}

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
	vm->native_libraries = lt_buffer_new(sizeof(lt_NativeLibrary));
	ltasync_init_state(vm);

	vm->generate_debug = 1;

	vm->global = LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_TABLE));
	lt_nocollect(vm, LT_GET_OBJECT(vm->global));

	return vm;
}

void lt_destroy(lt_VM* vm)
{
	ltasync_destroy_state(vm);
	for (uint32_t i = 0; i < vm->native_libraries.length; ++i)
	{
		lt_NativeLibrary* library = lt_buffer_at(&vm->native_libraries, i);
		if (library->handle && library->close) library->close(library->handle);
	}
	lt_buffer_destroy(vm, &vm->native_libraries);
	lt_buffer_destroy(vm, &vm->keepalive);
	lt_collect(vm);
	if (vm->error_trap) vm->free(vm->error_trap);
	vm->free(vm);
}

lt_Object* lt_allocate(lt_VM* vm, lt_ObjectType type)
{
	lt_Object* obj = vm->alloc(sizeof(lt_Object));
	memset(obj, 0, sizeof(lt_Object));
	obj->type = type;

	lt_buffer_push(vm, &vm->heap, &obj);

	return obj;
}

void lt_free(lt_VM* vm, uint32_t heapidx)
{
	lt_Object* obj = *(lt_Object**)lt_buffer_at(&vm->heap, heapidx);

	switch (obj->type)
	{
	case LT_OBJECT_CHUNK: {
		lt_buffer_destroy(vm, &obj->chunk.code);
		lt_buffer_destroy(vm, &obj->chunk.constants);
	} break;
	case LT_OBJECT_CLOSURE: {
		lt_buffer_destroy(vm, &obj->closure.captures);
	} break;
	case LT_OBJECT_FN: {
		lt_buffer_destroy(vm, &obj->fn.code);
		lt_buffer_destroy(vm, &obj->fn.constants);
	} break;
	case LT_OBJECT_TABLE: {
		for (uint8_t i = 0; i < 16; ++i)
			lt_buffer_destroy(vm, obj->table.buckets + i);
	} break;
	case LT_OBJECT_ARRAY: {
		lt_buffer_destroy(vm, &obj->array);
	} break;
	case LT_OBJECT_PROMISE: {
		ltasync_free_promise(vm, obj);
	} break;
	case LT_OBJECT_CLASS: {
		_lt_table_destroy(vm, &obj->class_def.public_fields);
		_lt_table_destroy(vm, &obj->class_def.private_fields);
		_lt_table_destroy(vm, &obj->class_def.public_methods);
		_lt_table_destroy(vm, &obj->class_def.private_methods);
		_lt_table_destroy(vm, &obj->class_def.public_getters);
		_lt_table_destroy(vm, &obj->class_def.private_getters);
		_lt_table_destroy(vm, &obj->class_def.public_setters);
		_lt_table_destroy(vm, &obj->class_def.private_setters);
	} break;
	case LT_OBJECT_INSTANCE: {
		_lt_table_destroy(vm, &obj->instance.public_fields);
		_lt_table_destroy(vm, &obj->instance.private_fields);
	} break;
	case LT_OBJECT_CELL: {
	} break;
	case LT_OBJECT_PTR: {
		vm->free(obj->ptr);
	} break;
	case LT_OBJECT_SHARED_TABLE:
	case LT_OBJECT_SHARED_ARRAY: {
		ltshared_release(obj->shared);
	} break;
	}

	lt_buffer_cycle(&vm->heap, heapidx);
	vm->free(obj);
}

void lt_nocollect(lt_VM* vm, lt_Object* obj)
{
	lt_buffer_push(vm, &vm->keepalive, &obj);
}

void lt_resumecollect(lt_VM* vm, lt_Object* obj)
{
	for (uint32_t i = 0; i < vm->keepalive.length; i++)
	{
		if ((*(lt_Object**)lt_buffer_at(&vm->keepalive, i)) == obj)
		{
			lt_buffer_cycle(&vm->keepalive, i);
			return;
		}
	}
}

#define MARK(x) (x->markbit = 1)
#define CLEAR(x) (x->markbit = 0)

void lt_sweep(lt_VM* vm, lt_Object* obj);

void lt_sweep_v(lt_VM* vm, lt_Value val)
{
	if (LT_IS_OBJECT(val)) lt_sweep(vm, LT_GET_OBJECT(val));
	else if (LT_IS_STRING(val)) _lt_reference_string(vm, val);
}

void lt_sweep(lt_VM* vm, lt_Object* obj)
{
	if (!obj || !obj->markbit) return;
	CLEAR(obj);
	switch (obj->type)
	{
	case LT_OBJECT_CHUNK: {
		for (uint32_t i = 0; i < obj->chunk.constants.length; ++i)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->chunk.constants, i));
		}
	} break;
	case LT_OBJECT_CLOSURE: {
		lt_sweep_v(vm, obj->closure.function);
		if (obj->closure.owner_class) lt_sweep(vm, obj->closure.owner_class);
		for (uint32_t i = 0; i < obj->closure.captures.length; ++i)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->closure.captures, i));
		}
	} break;
	case LT_OBJECT_CELL: {
		lt_sweep_v(vm, obj->cell);
	} break;
	case LT_OBJECT_BOUND_NATIVE: {
		lt_sweep_v(vm, obj->bound_native.receiver);
	} break;
	case LT_OBJECT_FN: {
		if (obj->fn.owner_class) lt_sweep(vm, obj->fn.owner_class);
		for (uint32_t i = 0; i < obj->fn.constants.length; ++i)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->fn.constants, i));
		}
	} break;
	case LT_OBJECT_TABLE: {
		for (uint16_t i = 0; i < 16; ++i)
		{
			lt_Buffer* bucket = obj->table.buckets + i;
			for (uint32_t j = 0; j < bucket->length; ++j)
			{
				lt_sweep_v(vm, ((lt_TablePair*)lt_buffer_at(bucket, j))->key);
				lt_sweep_v(vm, ((lt_TablePair*)lt_buffer_at(bucket, j))->value);
			}
		}
	} break;
	case LT_OBJECT_ARRAY: {
		for (uint32_t j = 0; j < obj->array.length; ++j)
		{
			lt_sweep_v(vm, *(lt_Value*)lt_buffer_at(&obj->array, j));
		}
	} break;
	case LT_OBJECT_PROMISE: {
		ltasync_mark_promise(vm, obj);
	} break;
	case LT_OBJECT_CLASS: {
		if (obj->class_def.superclass) lt_sweep(vm, obj->class_def.superclass);
		lt_sweep_v(vm, obj->class_def.name);
		lt_sweep_v(vm, obj->class_def.constructor);
		_lt_table_mark(vm, &obj->class_def.public_fields);
		_lt_table_mark(vm, &obj->class_def.private_fields);
		_lt_table_mark(vm, &obj->class_def.public_methods);
		_lt_table_mark(vm, &obj->class_def.private_methods);
		_lt_table_mark(vm, &obj->class_def.public_getters);
		_lt_table_mark(vm, &obj->class_def.private_getters);
		_lt_table_mark(vm, &obj->class_def.public_setters);
		_lt_table_mark(vm, &obj->class_def.private_setters);
	} break;
	case LT_OBJECT_INSTANCE: {
		lt_sweep(vm, obj->instance.klass);
		_lt_table_mark(vm, &obj->instance.public_fields);
		_lt_table_mark(vm, &obj->instance.private_fields);
	} break;
	case LT_OBJECT_SHARED_TABLE:
	case LT_OBJECT_SHARED_ARRAY: {
	} break;
	}
}

uint32_t lt_collect(lt_VM* vm)
{
	uint32_t num_collected = 0;

	for (uint32_t i = 0; i < vm->heap.length; ++i)
	{
		lt_Object* obj = *(lt_Object**)lt_buffer_at(&vm->heap, i);
		MARK(obj);
	}

	for (uint32_t i = 0; i < LT_DEDUP_TABLE_SIZE; i++)
	{
		for (uint32_t j = 0; j < vm->strings[i].length; ++j)
		{
			lt_StringDedupEntry* e = lt_buffer_at(vm->strings + i, j);
			e->refcount = 0;
		}
	}

	for (uint32_t i = 0; i < vm->keepalive.length; ++i)
	{
		lt_sweep(vm, *(lt_Object**)lt_buffer_at(&vm->keepalive, i));
	}

	for (uint32_t i = 0; i < vm->top; ++i)
	{
		lt_sweep_v(vm, vm->stack[i]);
	}

	ltasync_mark_roots(vm);

	for (uint32_t i = 0; i < vm->heap.length; ++i)
	{
		lt_Object* obj = *(lt_Object**)lt_buffer_at(&vm->heap, i);
		if (obj->markbit)
		{
			lt_free(vm, i--);
			num_collected++;
		}
	}

	for (uint32_t i = 0; i < LT_DEDUP_TABLE_SIZE; i++)
	{
		for (uint32_t j = 0; j < vm->strings[i].length; ++j)
		{
			lt_StringDedupEntry* e = lt_buffer_at(vm->strings + i, j);
			if (e->refcount == 0 && e->hash != 0)
			{
				vm->free(e->string);
				e->hash = 0; // mark for reopen
			}
		}
	}

	return num_collected;
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

static void _lt_table_copy(lt_VM* vm, lt_Table* dst, lt_Table* src)
{
	_lt_table_init(dst);
	for (uint8_t i = 0; i < 16; ++i)
	{
		lt_Buffer* bucket = src->buckets + i;
		for (uint32_t j = 0; j < bucket->length; ++j)
		{
			lt_TablePair* pair = lt_buffer_at(bucket, j);
			_lt_table_set_raw(vm, dst, pair->key, pair->value);
		}
	}
}

static uint8_t _lt_has_class_access(lt_VM* vm, lt_Object* klass)
{
	return vm->current && vm->current->class_context == klass;
}

static lt_Table* _lt_instance_private_fields_for(lt_VM* vm, lt_Object* instance, lt_Object* klass, uint8_t create)
{
	lt_Value key = LT_VALUE_OBJECT(klass);
	lt_Value fields = _lt_table_get_raw(&instance->instance.private_fields, key);
	if (!LT_IS_TABLE(fields))
	{
		if (!create) return 0;
		fields = lt_make_table(vm);
		_lt_table_set_raw(vm, &instance->instance.private_fields, key, fields);
	}
	return &LT_GET_OBJECT(fields)->table;
}

static lt_Object* _lt_superclass_of(lt_Object* klass)
{
	return klass ? klass->class_def.superclass : 0;
}

static void _lt_class_set_member(lt_VM* vm, lt_Value class_value, lt_Value key, lt_Value value, int16_t encoded)
{
	lt_Object* klass = LT_GET_OBJECT(class_value);
	lt_Object* member_obj = LT_IS_OBJECT(value) ? LT_GET_OBJECT(value) : 0;
	if (member_obj && member_obj->type == LT_OBJECT_CLOSURE) member_obj->closure.owner_class = klass;
	else if (member_obj && member_obj->type == LT_OBJECT_FN) member_obj->fn.owner_class = klass;

	lt_ClassMemberType type = (lt_ClassMemberType)(encoded & 0x0F);
	lt_Visibility visibility = (encoded & 0x10) ? LT_VIS_PRIVATE : LT_VIS_PUBLIC;
	uint8_t is_override = (encoded & 0x20) != 0;

	if (type == LT_CLASS_CONSTRUCTOR)
	{
		klass->class_def.constructor = value;
		return;
	}

	if (visibility == LT_VIS_PUBLIC)
	{
		uint8_t found_any_in_super = 0;
		uint8_t found_same_in_super = 0;
		for (lt_Object* current = _lt_superclass_of(klass); current; current = _lt_superclass_of(current))
		{
			if (_lt_table_index_raw(vm, &current->class_def.public_fields, key, 0) ||
				_lt_table_index_raw(vm, &current->class_def.public_getters, key, 0) ||
				_lt_table_index_raw(vm, &current->class_def.public_setters, key, 0) ||
				_lt_table_index_raw(vm, &current->class_def.public_methods, key, 0))
				found_any_in_super = 1;

			lt_Table* same_table = type == LT_CLASS_FIELD ? &current->class_def.public_fields :
				type == LT_CLASS_GETTER ? &current->class_def.public_getters :
				type == LT_CLASS_SETTER ? &current->class_def.public_setters : &current->class_def.public_methods;
			if (_lt_table_index_raw(vm, same_table, key, 0))
			{
				found_same_in_super = 1;
			}
		}
		if (found_any_in_super && !is_override) lt_runtime_error(vm, "Class member conflicts with inherited member; use override!");
		if (!found_same_in_super && is_override) lt_runtime_error(vm, "Override member has no matching inherited member!");
	}
	else if (is_override) lt_runtime_error(vm, "Private members cannot be override!");

	lt_Table* table = 0;
	if (type == LT_CLASS_FIELD) table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_fields : &klass->class_def.public_fields;
	else if (type == LT_CLASS_GETTER) table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_getters : &klass->class_def.public_getters;
	else if (type == LT_CLASS_SETTER) table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_setters : &klass->class_def.public_setters;
	else table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_methods : &klass->class_def.public_methods;

	_lt_table_set_raw(vm, table, key, value);
}

static lt_Value _lt_make_class(lt_VM* vm, lt_Value name)
{
	lt_Object* klass = lt_allocate(vm, LT_OBJECT_CLASS);
	klass->class_def.name = name;
	_lt_table_init(&klass->class_def.public_fields);
	_lt_table_init(&klass->class_def.private_fields);
	_lt_table_init(&klass->class_def.public_methods);
	_lt_table_init(&klass->class_def.private_methods);
	_lt_table_init(&klass->class_def.public_getters);
	_lt_table_init(&klass->class_def.private_getters);
	_lt_table_init(&klass->class_def.public_setters);
	_lt_table_init(&klass->class_def.private_setters);
	klass->class_def.constructor = LT_VALUE_NULL;
	return LT_VALUE_OBJECT(klass);
}

static void _lt_class_set_superclass(lt_VM* vm, lt_Value class_value, lt_Value superclass_value)
{
	if (!LT_IS_CLASS(class_value) || !LT_IS_CLASS(superclass_value)) lt_runtime_error(vm, "Expected class superclass!");
	lt_Object* klass = LT_GET_OBJECT(class_value);
	lt_Object* superclass = LT_GET_OBJECT(superclass_value);
	for (lt_Object* current = superclass; current; current = _lt_superclass_of(current))
		if (current == klass) lt_runtime_error(vm, "Class inheritance cycle!");
	klass->class_def.superclass = superclass;
}

static lt_Value _lt_make_instance(lt_VM* vm, lt_Object* klass)
{
	lt_Object* instance = lt_allocate(vm, LT_OBJECT_INSTANCE);
	instance->instance.klass = klass;
	_lt_table_init(&instance->instance.public_fields);
	_lt_table_init(&instance->instance.private_fields);
	return LT_VALUE_OBJECT(instance);
}

static void _lt_run_field_initializers(lt_VM* vm, lt_Value instance, lt_Table* initializers, lt_Table* fields)
{
	for (uint8_t i = 0; i < 16; ++i)
	{
		lt_Buffer* bucket = initializers->buckets + i;
		for (uint32_t j = 0; j < bucket->length; ++j)
		{
			lt_TablePair* pair = lt_buffer_at(bucket, j);
			lt_push(vm, instance);
			uint16_t nret = lt_exec_internal(vm, pair->value, 1);
			lt_Value value = LT_VALUE_NULL;
			if (nret > 0)
			{
				value = lt_pop(vm);
				for (uint16_t r = 1; r < nret; ++r) lt_pop(vm);
			}
			_lt_table_set_raw(vm, fields, pair->key, value);
		}
	}
}

static void _lt_run_class_field_initializers(lt_VM* vm, lt_Value instance, lt_Object* klass)
{
	if (klass->class_def.superclass) _lt_run_class_field_initializers(vm, instance, klass->class_def.superclass);
	_lt_run_field_initializers(vm, instance, &klass->class_def.public_fields, &LT_GET_OBJECT(instance)->instance.public_fields);
	lt_Table* private_fields = _lt_instance_private_fields_for(vm, LT_GET_OBJECT(instance), klass, 1);
	_lt_run_field_initializers(vm, instance, &klass->class_def.private_fields, private_fields);
}

static lt_Value _lt_class_call(lt_VM* vm, lt_Value class_value, uint8_t argc)
{
	lt_Object* klass = LT_GET_OBJECT(class_value);
	lt_Value instance = _lt_make_instance(vm, klass);
	lt_nocollect(vm, LT_GET_OBJECT(instance));
	uint16_t base = vm->top - argc;

	_lt_run_class_field_initializers(vm, instance, klass);

	if (!LT_IS_NULL(klass->class_def.constructor))
	{
		for (uint8_t i = 0; i < argc; ++i)
			vm->stack[vm->top - i] = vm->stack[vm->top - i - 1];
		vm->stack[base] = instance;
		vm->top++;
		if (vm->top > LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");

		uint16_t nret = lt_exec_internal(vm, klass->class_def.constructor, argc + 1);
		while (nret-- > 0) lt_pop(vm);
	}
	else if (klass->class_def.superclass && !LT_IS_NULL(klass->class_def.superclass->class_def.constructor))
	{
		for (uint8_t i = 0; i < argc; ++i)
			vm->stack[vm->top - i] = vm->stack[vm->top - i - 1];
		vm->stack[base] = instance;
		vm->top++;
		if (vm->top > LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");
		uint16_t nret = lt_exec_internal(vm, klass->class_def.superclass->class_def.constructor, argc + 1);
		while (nret-- > 0) lt_pop(vm);
	}

	vm->top = base;
	lt_resumecollect(vm, LT_GET_OBJECT(instance));
	return instance;
}

static lt_Value _lt_instance_get(lt_VM* vm, lt_Value instance_value, lt_Value key)
{
	lt_Object* instance = LT_GET_OBJECT(instance_value);
	lt_Object* klass = instance->instance.klass;
	lt_Value result = LT_VALUE_NULL;

	if (vm->current && vm->current->class_context)
	{
		lt_Object* access = vm->current->class_context;
		lt_TablePair* private_getter = _lt_table_index_raw(vm, &access->class_def.private_getters, key, 0);
		if (private_getter)
		{
			lt_push(vm, instance_value);
			uint16_t nret = lt_exec_internal(vm, private_getter->value, 1);
			if (nret > 0)
			{
				result = vm->stack[vm->top - nret];
				while (nret-- > 0) lt_pop(vm);
			}
			return result;
		}
	}

	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
	{
		lt_TablePair* public_getter = _lt_table_index_raw(vm, &current->class_def.public_getters, key, 0);
		if (public_getter)
		{
			lt_push(vm, instance_value);
			uint16_t nret = lt_exec_internal(vm, public_getter->value, 1);
			if (nret > 0)
			{
				result = vm->stack[vm->top - nret];
				while (nret-- > 0) lt_pop(vm);
			}
			return result;
		}
	}

	if (vm->current && vm->current->class_context)
	{
		lt_Table* private_fields = _lt_instance_private_fields_for(vm, instance, vm->current->class_context, 0);
		lt_TablePair* private_field = private_fields ? _lt_table_index_raw(vm, private_fields, key, 0) : 0;
		if (private_field) return private_field->value;
	}

	lt_TablePair* public_field = _lt_table_index_raw(vm, &instance->instance.public_fields, key, 0);
	if (public_field) return public_field->value;

	if (vm->current && vm->current->class_context)
	{
		lt_Object* access = vm->current->class_context;
		lt_TablePair* private_method = _lt_table_index_raw(vm, &access->class_def.private_methods, key, 0);
		if (private_method) return private_method->value;
	}

	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
	{
		lt_TablePair* public_method = _lt_table_index_raw(vm, &current->class_def.public_methods, key, 0);
		if (public_method) return public_method->value;
	}

	return LT_VALUE_NULL;
}

static void _lt_instance_set(lt_VM* vm, lt_Value instance_value, lt_Value key, lt_Value value)
{
	lt_Object* instance = LT_GET_OBJECT(instance_value);
	lt_Object* klass = instance->instance.klass;
	if (vm->current && vm->current->class_context)
	{
		lt_Object* access = vm->current->class_context;
		lt_TablePair* private_setter = _lt_table_index_raw(vm, &access->class_def.private_setters, key, 0);
		if (private_setter)
		{
			lt_push(vm, instance_value);
			lt_push(vm, value);
			uint16_t nret = lt_exec_internal(vm, private_setter->value, 2);
			while (nret-- > 0) lt_pop(vm);
			return;
		}
	}

	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
	{
		lt_TablePair* public_setter = _lt_table_index_raw(vm, &current->class_def.public_setters, key, 0);
		if (public_setter)
		{
			lt_push(vm, instance_value);
			lt_push(vm, value);
			uint16_t nret = lt_exec_internal(vm, public_setter->value, 2);
			while (nret-- > 0) lt_pop(vm);
			return;
		}
	}

	if (vm->current && vm->current->class_context)
	{
		lt_Table* private_fields = _lt_instance_private_fields_for(vm, instance, vm->current->class_context, 0);
		if (private_fields && _lt_table_index_raw(vm, private_fields, key, 0))
		{
			_lt_table_set_raw(vm, private_fields, key, value);
			return;
		}
	}

	_lt_table_set_raw(vm, &instance->instance.public_fields, key, value);
}

uint16_t lt_exec(lt_VM* vm, lt_Value callable, uint8_t argc)
{
	uint16_t base = vm->top - argc;
	uint16_t saved_depth = vm->depth;
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
		vm->error_buf = saved_error_buf;
		return 0;
	}
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

uint16_t lt_exec_internal(lt_VM* vm, lt_Value callable, uint8_t argc)
{
	if (!LT_IS_OBJECT(callable))
	{
		lt_runtime_error(vm, "Value is not callable!");
		return 0;
	}
	if (LT_IS_CLASS(callable))
	{
		lt_push(vm, _lt_class_call(vm, callable, argc));
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
			uint8_t n_return = fn->native(vm, argc);

			--vm->depth;
			vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
			return n_return;
		}
	} break;
	case LT_OBJECT_NATIVEFN: {
		uint8_t n_return = callee->native(vm, argc);

		--vm->depth;
		vm->current = vm->depth > 0 ? &vm->callstack[vm->depth - 1] : 0;
		return n_return;
	} break;
	case LT_OBJECT_BOUND_NATIVE: {
		uint8_t n_return = callee->bound_native.native(vm, argc);

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
		PUSH(_lt_make_class(vm, POP()));
	} NEXT;

	case LT_OP_SETSUPER: {
		lt_Value superclass = POP();
		lt_Value klass = POP();
		_lt_class_set_superclass(vm, klass, superclass);
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
			_lt_instance_set(vm, t, key, value);
		}
		else {};
	} NEXT;

	case LT_OP_SETC: {
		lt_Value key = POP();
		lt_Value klass = POP();
		lt_Value value = POP();
		_lt_class_set_member(vm, klass, key, value, current.arg);
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
			PUSH(_lt_instance_get(vm, t, key));
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
			PUSH(_lt_instance_get(vm, t, key));
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
			lt_Value result = _lt_class_call(vm, callee, (uint8_t)current.arg);
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
			lt_Value result = _lt_class_call(vm, callee, argc);
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
		if (LT_IS_NULL(superclass->class_def.constructor)) lt_runtime_error(vm, "Superclass has no constructor!");
		uint8_t argc = (uint8_t)current.arg;
		uint16_t base = vm->top - argc;
		for (uint8_t i = 0; i < argc; ++i)
			vm->stack[vm->top - i] = vm->stack[vm->top - i - 1];
		vm->stack[base] = vm->stack[frame->start];
		vm->top++;
		if (vm->top > LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");
		uint16_t nret = lt_exec_internal(vm, superclass->class_def.constructor, argc + 1);
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
			method = _lt_table_index_raw(vm, &current_class->class_def.public_methods, key, 0);
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

	case LT_OP_JMP: frame->pc += current.arg; NEXT;
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

uint16_t _lt_push_constant(lt_VM* vm, lt_Buffer* constants, lt_Value constant)
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
uint16_t _lt_push_constant(lt_VM* vm, lt_Buffer* constants, lt_Value constant);
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

	lt_Op op = { LT_OP_PUSH, 0 };
	lt_buffer_push(vm, &fn->fn.code, &op);

	_lt_compile_body(vm, p, name, &fn->fn.debug->locations, &node->fn.body, node->fn.scope, &fn->fn.code, &fn->fn.constants);

	lt_Op op2 = { LT_OP_RET, 0 };
	lt_buffer_push(vm, &fn->fn.code, &op2);

	((lt_Op*)lt_buffer_at(&fn->fn.code, 0))->arg = node->fn.scope->locals.length;
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
			uint32_t idx = _lt_find_local(vm, scope, (lt_Token*)lt_buffer_at(upvals, i));
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
		}
	} break;
	case LT_AST_NODE_BREAK: {
		OPARG(JMP, 0);
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
		uint32_t idx = _lt_find_local(vm, scope, node->identifier.token);
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
		else if (_lt_is_captured_local(scope, idx))
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
			uint16_t idx = _lt_make_local(vm, scope, node->declare.identifier);
			if (node->declare.expr)
			{
				_lt_compile_node(vm, p, name, debug, node->declare.expr, scope, code_body, constants);
				if (_lt_is_captured_local(scope, idx)) OPARG(STORECELL, idx)
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
					_lt_compile_push_token_value(vm, p, _lt_make_number_token(vm, p, (double)i, entry->local), debug, code_body, constants, &node->loc);
				else
					_lt_compile_push_token_value(vm, p, entry->key, debug, code_body, constants, &node->loc);
				OP(GETD);
				uint32_t idx = _lt_find_local(vm, scope, entry->local);
				if (_lt_is_captured_local(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
				else OPARG(STORE, idx & 0xFFFF);
			}
			OPARG(POP, 1);
		}
	} break;

	case LT_AST_NODE_CLASS: {
		lt_Value class_name = lt_make_string(vm, ((lt_Identifier*)lt_buffer_at(&p->tkn->identifier_buffer, node->class_decl.identifier->idx))->name);
		uint32_t idx = _lt_find_local(vm, scope, node->class_decl.identifier);
		if (idx == NOT_FOUND) idx = _lt_make_local(vm, scope, node->class_decl.identifier);

		OPARG(PUSHC, _lt_push_constant(vm, constants, class_name));
		OP(MAKEC);
		if (_lt_is_captured_local(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
		else OPARG(STORE, idx & 0xFFFF);

		if (node->class_decl.superclass)
		{
			if (_lt_is_captured_local(scope, idx)) OPARG(LOADCELL, idx & 0xFFFF)
			else OPARG(LOAD, idx & 0xFFFF);
			uint32_t super_idx = _lt_find_local(vm, scope, node->class_decl.superclass);
			if (super_idx == NOT_FOUND) _lt_parse_error(vm, name, node->class_decl.superclass, "Can't find superclass!");
			else if ((super_idx & UPVAL_BIT) == UPVAL_BIT) OPARG(LOADUP, super_idx & 0xFFFF)
			else if (_lt_is_captured_local(scope, super_idx)) OPARG(LOADCELL, super_idx & 0xFFFF)
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
			if (_lt_is_captured_local(scope, idx)) OPARG(LOADCELL, idx & 0xFFFF)
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
			uint32_t idx = _lt_find_local(vm, scope, target->identifier.token);
			if (idx == NOT_FOUND) _lt_parse_error(vm, name, target->identifier.token, "Can't find local to assign to!");
			else if ((idx & UPVAL_BIT) == UPVAL_BIT) OPARG(STOREUP, idx & 0xFFFF)
			else if (_lt_is_captured_local(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
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
				uint32_t idx = _lt_find_local(vm, scope, (lt_Token*)lt_buffer_at(upvals, i));
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
		if (node->ret.expr)
		{
			if (node->ret.expr->type == LT_AST_NODE_CALL)
			{
				_lt_compile_node_ex(vm, p, name, debug, node->ret.expr, scope, code_body, constants, 1);
				OP(RETM);
			}
			else
			{
				_lt_compile_node(vm, p, name, debug, node->ret.expr, scope, code_body, constants);
				OPARG(RET, 1);
			}
		}
		else OP(RET);
	} break;

#define REG_JMP() { if (n_branches >= LT_MAX_BRANCHES) lt_error(vm, "Too many if/elseif branches!"); branch_stack[n_branches++] = code_body->length; OP(NOP); }

	case LT_AST_NODE_IF: {
		uint8_t n_branches = 0;
		uint32_t branch_stack[LT_MAX_BRANCHES];

		_lt_compile_node(vm, p, name, debug, node->branch.expr, scope, code_body, constants);
		uint32_t jidx = code_body->length;
		OP(JMPC);

		_lt_compile_body(vm, p, name, debug, &node->branch.body, scope, code_body, constants);
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

				_lt_compile_body(vm, p, name, debug, &next->branch.body, scope, code_body, constants);
				REG_JMP();

				((lt_Op*)lt_buffer_at(code_body, jidx))->arg = code_body->length - jidx - 1;
			}
			else
			{
				has_else = 1;

				_lt_compile_body(vm, p, name, debug, &next->branch.body, scope, code_body, constants);
			}

			next = next->branch.next;
		}

		if (has_elseif || has_else)
		{
			for (uint32_t i = 0; i < n_branches; i++)
			{
				uint32_t loc = branch_stack[i];
				*((lt_Op*)lt_buffer_at(code_body, loc)) = (lt_Op) { LT_OP_JMP, code_body->length - loc - 1 };
			}
		}
	} break;

	case LT_AST_NODE_FOR: {
		_lt_compile_node(vm, p, name, debug, node->loop.iterator, scope, code_body, constants);
		OPARG(STORE, node->loop.closureidx);

		uint32_t loop_header = code_body->length;
		OPARG(LOAD, node->loop.closureidx);
		OPARG(CALL, 0);
		if (_lt_is_captured_local(scope, node->loop.identifier)) OPARG(STORECELL, node->loop.identifier)
		else OPARG(STORE, node->loop.identifier);
		if (_lt_is_captured_local(scope, node->loop.identifier)) OPARG(LOADCELL, node->loop.identifier)
		else OPARG(LOAD, node->loop.identifier);
		uint32_t loop_start = code_body->length;
		OPARG(JMPN, 0);

		_lt_compile_body(vm, p, name, debug, &node->loop.body, scope, code_body, constants);
		OPARG(JMP, loop_header - code_body->length - 1);

		lt_Op* cond = lt_buffer_at(code_body, loop_start);
		cond->arg = code_body->length - loop_start - 1;

		for (uint32_t i = loop_start; i < code_body->length; ++i)
		{
			lt_Op* current = lt_buffer_at(code_body, i);
			if (current->op == LT_OP_JMP && current->arg == 0)
				current->arg = code_body->length - i - 1;
		}
	} break;

	case LT_AST_NODE_WHILE: {
		uint32_t loop_header = code_body->length;
		_lt_compile_node(vm, p, name, debug, node->loop.iterator, scope, code_body, constants);
		uint32_t loop_start = code_body->length;
		OPARG(JMPC, 0);

		_lt_compile_body(vm, p, name, debug, &node->loop.body, scope, code_body, constants);
		OPARG(JMP, loop_header - code_body->length - 1);

		lt_Op* cond = lt_buffer_at(code_body, loop_start);
		cond->arg = code_body->length - loop_start - 1;

		for (uint32_t i = loop_start; i < code_body->length; ++i)
		{
			lt_Op* current = lt_buffer_at(code_body, i);
			if (current->op == LT_OP_JMP && current->arg == 0)
				current->arg = code_body->length - i - 1;
		}
	} break;

	case LT_AST_NODE_WITH: {
		_lt_compile_node(vm, p, name, debug, node->with_stmt.expr, scope, code_body, constants);
		uint32_t idx = _lt_find_local(vm, scope, node->with_stmt.receiver);
		if (idx == NOT_FOUND) idx = _lt_make_local(vm, scope, node->with_stmt.receiver);
		if (_lt_is_captured_local(scope, idx)) OPARG(STORECELL, idx & 0xFFFF)
		else OPARG(STORE, idx & 0xFFFF);
		_lt_compile_body(vm, p, name, debug, &node->with_stmt.body, scope, code_body, constants);
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

	lt_Op op = { LT_OP_PUSH, 0 };
	lt_buffer_push(vm, &chunk->chunk.code, &op);

	_lt_compile_body(vm, p, chunk->chunk.name, &chunk->chunk.debug->locations, &p->root->chunk.body, p->root->chunk.scope, &chunk->chunk.code, &chunk->chunk.constants);
	
	lt_Op op2 = { LT_OP_RET, 0 };
	lt_buffer_push(vm, &chunk->chunk.code, &op2);

	((lt_Op*)lt_buffer_at(&chunk->chunk.code, 0))->arg = p->root->chunk.scope->locals.length;

	lt_Value as_val = LT_VALUE_OBJECT(chunk);
	return as_val;
}

void lt_free_scope(lt_VM* vm, lt_Scope* scope)
{
	lt_buffer_destroy(vm, &scope->locals);
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
			if (entry->chunk.scope) lt_free_scope(vm, entry->chunk.scope);
			break;
		case LT_AST_NODE_CLASS:
			lt_buffer_destroy(vm, &entry->class_decl.members);
			break;
		case LT_AST_NODE_DECLARE:
			if (entry->declare.destructure != LT_DESTRUCT_NONE) lt_buffer_destroy(vm, &entry->declare.entries);
			break;
		case LT_AST_NODE_TABLE: lt_buffer_destroy(vm, &entry->table.keys); lt_buffer_destroy(vm, &entry->table.values); break;
		case LT_AST_NODE_ARRAY: lt_buffer_destroy(vm, &entry->array.values); break;
		case LT_AST_NODE_FN:
			if (entry->fn.scope) lt_free_scope(vm, entry->fn.scope);
			break;
		case LT_AST_NODE_IF: case LT_AST_NODE_ELSEIF: case LT_AST_NODE_ELSE: lt_buffer_destroy(vm, &entry->branch.body); break;
		case LT_AST_NODE_WITH: lt_buffer_destroy(vm, &entry->with_stmt.body); break;
		}

		vm->free(entry);
	}

	lt_buffer_destroy(vm, &p->ast_nodes);
}

void lt_free_tokenizer(lt_VM* vm, lt_Tokenizer* tok)
{
	lt_buffer_destroy(vm, &tok->token_buffer);

	for (uint32_t i = 0; i < tok->identifier_buffer.length; ++i)
		vm->free(((lt_Identifier*)lt_buffer_at(&tok->identifier_buffer, i))->name);
	lt_buffer_destroy(vm, &tok->identifier_buffer);


	for (uint32_t i = 0; i < tok->literal_buffer.length; ++i)
	{
		lt_Literal* lit = lt_buffer_at(&tok->literal_buffer, i);
		if (lit->type == LT_TOKEN_STRING_LITERAL)
		{
			vm->free(lit->string);
		}
	}
	lt_buffer_destroy(vm, &tok->literal_buffer);
}

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

#define HASH(x) (LT_IS_OBJECT(x) ? ((x >> 2) % 16) : (x % 16))

lt_TablePair* _lt_table_index(lt_VM* vm, lt_Value table, lt_Value key, uint8_t alloc)
{
	if (!LT_IS_OBJECT(table) || LT_GET_OBJECT(table)->type != LT_OBJECT_TABLE) return 0;
	uint8_t bucket = HASH(key);
	lt_Buffer* buf = LT_GET_OBJECT(table)->table.buckets + bucket;
	if (alloc && buf->element_size == 0) *buf = lt_buffer_new(sizeof(lt_TablePair));

	for (uint32_t i = 0; i < buf->length; i++)
	{
		lt_TablePair* p = lt_buffer_at(buf, i);
		if (lt_equals(p->key, key))
		{
			return p;
		}
	}

	return 0;
}

lt_Value lt_make_table(lt_VM* vm)
{
	return LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_TABLE));
}

lt_Value lt_table_set(lt_VM* vm, lt_Value table, lt_Value key, lt_Value val)
{
	if (!LT_IS_TABLE(table)) return LT_VALUE_NULL;
	if (LT_GET_OBJECT(table)->type == LT_OBJECT_SHARED_TABLE)
		return ltshared_table_set(vm, LT_GET_OBJECT(table)->shared, key, val);
	lt_TablePair* p = _lt_table_index(vm, table, key, 1);
	if (p)
	{
		p->value = val;
		return val;
	}

	uint8_t bucket = HASH(key);
	lt_Buffer* buf = LT_GET_OBJECT(table)->table.buckets + bucket;
	lt_TablePair newpair = { key, val };
	lt_buffer_push(vm, buf, &newpair);
	return val;
}

lt_Value lt_table_get(lt_VM* vm, lt_Value table, lt_Value key)
{
	if (!LT_IS_TABLE(table)) return LT_VALUE_NULL;
	if (LT_GET_OBJECT(table)->type == LT_OBJECT_SHARED_TABLE)
		return ltshared_table_get(vm, LT_GET_OBJECT(table)->shared, key);
	lt_TablePair* p = _lt_table_index(vm, table, key, 0);
	if (p) return p->value;
	return LT_VALUE_NULL;
}

uint8_t lt_table_next(lt_VM* vm, lt_Value table, uint64_t* cursor, lt_Value* key, lt_Value* val)
{
	/* Four bits select one of 16 buckets; the remaining 60 bits hold the pair index. */
	enum {
		LT_TABLE_CURSOR_BUCKET_BITS = 4,
		LT_TABLE_CURSOR_INDEX_BITS = 64 - LT_TABLE_CURSOR_BUCKET_BITS,
		LT_TABLE_CURSOR_BUCKET_COUNT = 1 << LT_TABLE_CURSOR_BUCKET_BITS
	};
	const uint64_t table_cursor_index_mask = (UINT64_C(1) << LT_TABLE_CURSOR_INDEX_BITS) - 1;

	if (!LT_IS_TABLE(table)) return 0;
	lt_Object* obj = LT_GET_OBJECT(table);
	if (obj->type == LT_OBJECT_SHARED_TABLE)
		return ltshared_table_next(vm, obj->shared, cursor, key, val);
	if (obj->type != LT_OBJECT_TABLE) return 0;

	uint64_t bucket = *cursor >> LT_TABLE_CURSOR_INDEX_BITS;
	uint64_t index = *cursor & table_cursor_index_mask;

	for (; bucket < LT_TABLE_CURSOR_BUCKET_COUNT; ++bucket)
	{
		lt_Buffer* buf = obj->table.buckets + bucket;
		if (index < buf->length)
		{
			lt_TablePair* pair = lt_buffer_at(buf, index);
			*key = pair->key;
			*val = pair->value;
			*cursor = (bucket << LT_TABLE_CURSOR_INDEX_BITS) | (index + 1);
			return 1;
		}
		index = 0;
	}

	return 0;
}

uint8_t lt_table_pop(lt_VM* vm, lt_Value table, lt_Value key)
{
	return lt_table_set(vm, table, key, LT_VALUE_NULL) == LT_VALUE_NULL;
}

lt_Value lt_make_array(lt_VM* vm)
{
	return LT_VALUE_OBJECT(lt_allocate(vm, LT_OBJECT_ARRAY));
}

lt_Value lt_array_push(lt_VM* vm, lt_Value array, lt_Value val)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_push(vm, arr->shared, val);
	if (arr->array.element_size == 0) arr->array = lt_buffer_new(sizeof(lt_Value));
	lt_buffer_push(vm, &arr->array, &val);
	return val;
}

lt_Value lt_array_get(lt_VM* vm, lt_Value array, uint32_t idx)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_get(vm, arr->shared, idx);
	return idx < arr->array.length ? *(lt_Value*)lt_buffer_at(&arr->array, idx) : LT_VALUE_NULL;
}

lt_Value lt_array_set(lt_VM* vm, lt_Value array, uint32_t idx, lt_Value val)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_set(vm, arr->shared, idx, val);
	if (idx >= arr->array.length) return LT_VALUE_NULL;
	*(lt_Value*)lt_buffer_at(&arr->array, idx) = val;
	return val;
}

lt_Value* lt_array_at(lt_Value array, uint32_t idx)
{
	if (!LT_IS_ARRAY(array) || LT_GET_OBJECT(array)->type == LT_OBJECT_SHARED_ARRAY) return &LT_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	return lt_buffer_at(&arr->array, idx);
}

lt_Value lt_array_remove(lt_VM* vm, lt_Value array, uint32_t idx)
{
	if (!LT_IS_ARRAY(array)) return LT_VALUE_NULL;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_remove(vm, arr->shared, idx);
	lt_Value old = *(lt_Value*)lt_buffer_at(&arr->array, idx);
	lt_buffer_cycle(&arr->array, idx);
	return old;
}

uint32_t lt_array_length(lt_Value array)
{
	if (!LT_IS_ARRAY(array)) return 0;
	lt_Object* arr = LT_GET_OBJECT(array);
	if (arr->type == LT_OBJECT_SHARED_ARRAY) return ltshared_array_length(arr->shared);
	return arr->array.length;
}

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
