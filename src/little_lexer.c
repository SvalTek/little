#include "little.h"
#include "little_buffer_internal.h"
#include "little_frontend_internal.h"
#include "little_value_internal.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
static uint16_t _lt_register_identifier_name(lt_VM* vm, lt_Tokenizer* t, const char* name, uint16_t length)
{
	for (uint32_t i = 0; i < t->identifier_buffer.length; i++)
	{
		lt_Identifier* id = lt_buffer_at(&t->identifier_buffer, i);
		if (faststrcmp(name, length, id->name, strlen(id->name)))
		{
			id->num_references++;
			return (uint16_t)i;
		}
	}

	lt_Identifier newid;
	newid.num_references = 1;
	newid.name = vm->alloc(length + 1);
	strncpy_s(newid.name, length + 1, name, length);
	newid.name[length] = 0;

	lt_buffer_push(vm, &t->identifier_buffer, &newid);
	return (uint16_t)(t->identifier_buffer.length - 1);
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
	t.comment_buffer = lt_buffer_new(sizeof(lt_Comment));

	void* saved_error_buf = vm->error_buf;
	jmp_buf error_buf;
	vm->error_buf = &error_buf;

	if (!setjmp(error_buf))
	{
		const char* current = source;
		uint16_t line = 1, col = 0;

#define PUSH_TOKEN_N(new_type, n) { \
	lt_Token _t; _t.type = new_type; _t.line = line; _t.col = col; _t.idx = 0; _t.flags = 0; \
	_t.span.start = (uint32_t)(token_start - source); _t.span.end = _t.span.start + (n); \
	col += (n); \
	lt_buffer_push(vm, &t.token_buffer, &_t); current += (n); found = 1;\
};
#define PUSH_TOKEN(type) PUSH_TOKEN_N(type, 1)
#define TOKEN_SPAN(a, b) ((lt_Span){ (uint32_t)((a) - source), (uint32_t)((b) - source) })

		while (*current)
		{
			const char* token_start = current;
			uint8_t found = 0;
			switch (*current)
			{
			case ' ': case '\t': { col++; current++; } found = 1; break;
			case '\n': { col = 0; line++; current++; } found = 1; break;
			case '\r': { current++; } found = 1; break;
			/* A comment ends at the newline or at the end of input. The old
			   loop scanned past the terminator when the comment was last. */
			case ';': {
				const char* comment_start = current;
				while (*current && *current != '\n') current++;

				lt_Comment comment;
				comment.line = line;
				comment.col = col;
				comment.span.start = (uint32_t)(comment_start - source);
				comment.span.end = (uint32_t)(current - source);
				lt_buffer_push(vm, &t.comment_buffer, &comment);

				if (*current == '\n') { current++; col = 0; line++; }
				found = 1;
			} break;
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
					if (*(current + 1) == '=') { PUSH_TOKEN_N(LT_TOKEN_GTE, 2) }
					else { PUSH_TOKEN(LT_TOKEN_GT) }
				}
				else if (*current == '<')
				{
					if (*(current + 1) == '=') { PUSH_TOKEN_N(LT_TOKEN_LTE, 2) }
					else { PUSH_TOKEN(LT_TOKEN_LT) }
				}
				else if (*current == '"')
				{
					uint16_t start_line = line;
					uint16_t start_col = col;
					current++;
					col++;
					const char* start = current;
					uint32_t raw_length = 0;
					while (*current != '"')
					{
						if (*current == 0) break;
						if (*current == '\\' && *(current + 1) != 0)
						{
							current += 2;
							raw_length += 2;
							col += 2;
							continue;
						}
						if (*current == '\n') { col = 0; line++; }
						else col++;
						current++;
						raw_length++;
					}

					const char* end = current;
					if (*current == '"') { current++; col++; }

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
					tok.line = start_line;
					tok.col = start_col;
					tok.idx = t.literal_buffer.length - 1;
					tok.flags = 0;
					tok.span.start = (uint32_t)(token_start - source);
					tok.span.end = (uint32_t)(current - source);
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
						if (!has_digits) lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Expected hex digits after 0x!");
						while (isxdigit(*current)) current++;
						if (*current == '.' || isalnum(*current) || *current == '_')
						{
							lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Invalid character in hex number literal!");
							while (*current == '.' || isalnum(*current) || *current == '_') current++;
						}

						length = (uint32_t)(current - start);
						char* end = 0;
						unsigned long long parsed = strtoull(start + 2, &end, 16);
						if (has_digits && end != current) lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Failed to parse hex number!");
						number = has_digits ? (double)parsed : 0;
					}
					else if (*current == '0' && (*(current + 1) == 'b' || *(current + 1) == 'B'))
					{
						current += 2;
						uint8_t has_digits = (*current == '0' || *current == '1');
						if (!has_digits) lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Expected binary digits after 0b!");
						while (*current == '0' || *current == '1') current++;
						if (*current == '.' || isalnum(*current) || *current == '_')
						{
							lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Invalid character in binary number literal!");
							while (*current == '.' || isalnum(*current) || *current == '_') current++;
						}

						length = (uint32_t)(current - start);
						double parsed = 0;
						for (const char* digit = start + 2; digit < current; ++digit) parsed = parsed * 2 + (*digit - '0');
						number = has_digits ? parsed : 0;
					}
					else
					{
						/* An exponent is only valid after digits, and only once,
						   so `1e` and `1e2e3` stay errors instead of silently
						   parsing as a shorter number. */
						uint8_t has_exponent = 0;
						while ((isalnum(*current) && !isalpha(*current)) || *current == '.' ||
							((*current == 'e' || *current == 'E') && !has_exponent) ||
							((*current == '+' || *current == '-') && has_exponent && (*(current - 1) == 'e' || *(current - 1) == 'E')))
						{
							if (*current == '.')
							{
								if (has_decimal) lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Can't have multiple decimals in number literal!");
								has_decimal = 1;
							}
							if (*current == 'e' || *current == 'E') has_exponent = 1;

							current++;
						}

						length = (uint32_t)(current - start);
						char* end = 0;
						number = strtod(start, &end);

						if (end != current) lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Failed to parse number!");
						/* A second exponent stops the scan without leaving the
						   token short, so it has to be rejected here or the
						   literal silently splits into a number and an
						   identifier. */
						if ((*current == 'e' || *current == 'E') && has_exponent)
							lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(start, current), "Can't have multiple exponents in number literal!");
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
					tok.flags = 0;
					tok.span.start = (uint32_t)(start - source);
					tok.span.end = (uint32_t)(current - source);
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
		lt_Token _t; _t.type = new_type; _t.line = line; _t.col = col; col += length; _t.idx = _lt_register_identifier_name(vm, &t, start, length); \
		_t.flags = 0; _t.span.start = (uint32_t)(start - source); _t.span.end = _t.span.start + length; \
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
							tok.flags = 0;
							tok.span.start = (uint32_t)(start - source);
							tok.span.end = tok.span.start + length;
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
						tok.flags = 0;
						tok.span.start = (uint32_t)(start - source);
						tok.span.end = tok.span.start + length;
						lt_buffer_push(vm, &t.token_buffer, &tok);
					}
				}
				}
				else lt_tokenize_error_internal(vm, t.module, line, col, TOKEN_SPAN(current, current + 1), "Unrecognized token!");
			}
		}

		lt_Token tok;
		tok.type = LT_TOKEN_END;
		tok.line = line;
		tok.col = col;
		tok.idx = 0;
		tok.flags = 0;
		tok.span.start = (uint32_t)(current - source);
		tok.span.end = tok.span.start;
		lt_buffer_push(vm, &t.token_buffer, &tok);

		t.is_valid = 1;
	}

	vm->error_buf = saved_error_buf;
	return t;
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
	lt_buffer_destroy(vm, &tok->comment_buffer);
}
