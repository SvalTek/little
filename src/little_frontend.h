#pragma once

#include "little_value.h"

typedef enum {
	LT_TOKEN_TRUE_LITERAL,
	LT_TOKEN_FALSE_LITERAL,
	LT_TOKEN_STRING_LITERAL,
	LT_TOKEN_NULL_LITERAL,
	LT_TOKEN_NUMBER_LITERAL,

	LT_TOKEN_IDENTIFIER,

	LT_TOKEN_PERIOD,
	LT_TOKEN_COMMA,
	LT_TOKEN_COLON,
	LT_TOKEN_AT,

	LT_TOKEN_OPENPAREN,
	LT_TOKEN_CLOSEPAREN,

	LT_TOKEN_OPENBRACKET,
	LT_TOKEN_CLOSEBRACKET,

	LT_TOKEN_OPENBRACE,
	LT_TOKEN_CLOSEBRACE,

	LT_TOKEN_FN,
	LT_TOKEN_ASYNC,
	LT_TOKEN_AWAIT,
	LT_TOKEN_CLASS,
	LT_TOKEN_EXTENDS,
	LT_TOKEN_OVERRIDE,
	LT_TOKEN_SUPER,
	LT_TOKEN_PUBLIC,
	LT_TOKEN_PRIVATE,
	LT_TOKEN_CONSTRUCTOR,
	LT_TOKEN_GET,
	LT_TOKEN_SET,
	LT_TOKEN_BREAK,
	LT_TOKEN_VAR,
	LT_TOKEN_GLOBAL,
	LT_TOKEN_IF,
	LT_TOKEN_ELSE,
	LT_TOKEN_ELSEIF,
	LT_TOKEN_FOR,
	LT_TOKEN_IN,
	LT_TOKEN_WHILE,
	LT_TOKEN_WITH,
	LT_TOKEN_IMPORT,
	LT_TOKEN_RETURN,

	LT_TOKEN_PLUS,
	LT_TOKEN_MINUS,
	LT_TOKEN_NEGATE,
	LT_TOKEN_MULTIPLY,
	LT_TOKEN_DIVIDE,
	LT_TOKEN_ASSIGN,
	LT_TOKEN_EQUALS,
	LT_TOKEN_NOTEQUALS,
	LT_TOKEN_GT,
	LT_TOKEN_GTE,
	LT_TOKEN_LT,
	LT_TOKEN_LTE,
	LT_TOKEN_AND,
	LT_TOKEN_OR,
	LT_TOKEN_NOT,
	LT_TOKEN_TYPE,
	LT_TOKEN_TYPEOF,

	LT_TOKEN_END,
} lt_TokenType;

/* Byte offsets into the analysed source: [start, end). */
typedef struct {
	uint32_t start, end;
} lt_Span;

/* Token was fabricated by the parser (hidden `with` names, implicit `this`,
   synthesized literals) rather than read from source. */
#define LT_TOKEN_SYNTHETIC 0x01

typedef struct {
	lt_TokenType type;
	uint16_t line, col, idx;
	lt_Span span;
	uint8_t flags;
} lt_Token;

/* Severity values match the language server protocol. */
typedef enum {
	LT_DIAG_ERROR = 1,
	LT_DIAG_WARNING = 2,
	LT_DIAG_INFO = 3,
	LT_DIAG_HINT = 4,
} lt_DiagnosticSeverity;

typedef struct {
	lt_Span span;
	uint16_t line, col;
	uint8_t severity;
	int32_t code;
	char* message;
} lt_Diagnostic;

typedef struct {
	lt_TokenType type;
	union {
		char* string;
		double number;
	};
} lt_Literal;

typedef struct {
	char* name;
	uint32_t num_references;
} lt_Identifier;

typedef struct {
	lt_Span span;
	uint16_t line, col;
} lt_Comment;

typedef struct {
	lt_Buffer token_buffer;
	lt_Buffer literal_buffer;
	lt_Buffer identifier_buffer;
	lt_Buffer comment_buffer;

	const char* source;
	const char* module;

	uint8_t is_valid;
} lt_Tokenizer;

typedef enum {
	LT_AST_NODE_ERROR,
	LT_AST_NODE_EMPTY,
	LT_AST_NODE_CHUNK,

	LT_AST_NODE_LITERAL,
	LT_AST_NODE_TABLE,
	LT_AST_NODE_ARRAY,
	LT_AST_NODE_IDENTIFIER,
	LT_AST_NODE_INDEX,
	LT_AST_NODE_BINARYOP,
	LT_AST_NODE_UNARYOP,
	LT_AST_NODE_DECLARE,
	LT_AST_NODE_CLASS,
	LT_AST_NODE_SUPER,
	LT_AST_NODE_ASSIGN,
	LT_AST_NODE_FN,
	LT_AST_NODE_CALL,
	LT_AST_NODE_AWAIT,
	LT_AST_NODE_RETURN,
	LT_AST_NODE_IF,
	LT_AST_NODE_ELSE,
	LT_AST_NODE_ELSEIF,
	LT_AST_NODE_FOR,
	LT_AST_NODE_WHILE,
	LT_AST_NODE_WITH,
	LT_AST_NODE_BREAK,
} lt_AstNodeType;

struct lt_AstNode;
struct lt_Scope;


typedef enum {
	LT_CLASS_FIELD,
	LT_CLASS_METHOD,
	LT_CLASS_CONSTRUCTOR,
	LT_CLASS_GETTER,
	LT_CLASS_SETTER,
} lt_ClassMemberType;

typedef struct {
	lt_ClassMemberType type;
	lt_Visibility visibility;
	lt_Token* name;
	struct lt_AstNode* value;
	uint8_t is_override;
} lt_ClassMember;

typedef enum {
	LT_DESTRUCT_NONE,
	LT_DESTRUCT_ARRAY,
	LT_DESTRUCT_TABLE,
} lt_DestructureType;

typedef struct {
	lt_Token* key;
	lt_Token* local;
} lt_DestructureEntry;

typedef struct lt_AstNode {
	lt_AstNodeType type;
	lt_DebugLoc loc;
	lt_Span span;
	lt_Token* end_token;

	union {
		struct {
			char* name;
			lt_Buffer body;
			struct lt_Scope* scope;
		} chunk;

		struct {
			lt_Token* token;
		} literal;

		struct {
			lt_Buffer keys;
			lt_Buffer values;
		} table;

		struct {
			lt_Buffer values;
		} array;

		struct {
			lt_Token* token;
		} identifier;

		struct {
			struct lt_AstNode* source;
			struct lt_AstNode* idx;
		} index;

		struct {
			lt_TokenType type;
			struct lt_AstNode* left;
			struct lt_AstNode* right;
		} binary_op;

		struct {
			lt_TokenType type;
			struct lt_AstNode* expr;
		} unary_op;

		struct {
			lt_Token* identifier;
			struct lt_AstNode* expr;
			lt_DestructureType destructure;
			lt_Buffer entries;
			uint8_t is_global;
		} declare;

		struct {
			lt_Token* identifier;
			lt_Token* superclass;
			lt_Buffer members;
		} class_decl;

		struct {
			lt_Token* method;
		} super_expr;

		struct {
			struct lt_AstNode* left;
			struct lt_AstNode* right;
		} assign;

		struct {
			lt_Token* args[LT_MAX_FUNCTION_PARAMS + 1];
			struct lt_Scope* scope;
			lt_Buffer body;
			uint8_t is_async;
		} fn;

		struct {
			struct lt_AstNode* callee;
			struct lt_AstNode* args[LT_MAX_CALL_ARGS + 2];
		} call;

		struct {
			struct lt_AstNode* expr;
		} await;

		struct {
			struct lt_AstNode* expr;
		} ret;

		struct {
			struct lt_AstNode* expr;
			lt_Buffer body;
			struct lt_AstNode* next;
		} branch;

		struct {
			uint16_t identifier, closureidx;
			struct lt_AstNode* iterator;
			lt_Buffer body;
		} loop;

		struct {
			lt_Token* receiver;
			struct lt_AstNode* expr;
			lt_Buffer body;
			struct lt_Scope* scope;
		} with_stmt;
	};
} lt_AstNode;

typedef struct lt_Scope {
	struct lt_Scope* last;
	struct lt_Scope* frame;

	lt_Token* start;
	lt_Buffer locals;
	lt_Buffer local_slots;
	lt_Buffer frame_slots;
	lt_Buffer upvals;
	lt_Buffer captured;
	lt_Token* end;
} lt_Scope;

typedef struct {
	lt_Buffer ast_nodes;
	lt_Buffer generated_tokens;
	lt_Buffer scopes;
	lt_AstNode* root;

	lt_Tokenizer* tkn;
	lt_Scope* current;

	uint8_t is_valid;
	uint8_t in_async;
	uint8_t in_constructor;
	uint8_t allow_table_call;
	uint8_t had_error;
	uint32_t next_with_id;
	lt_Token* self_token;
} lt_Parser;

/* Result of a non-executing analysis pass. Diagnostics live on the VM that
   produced them; free the analysis before destroying that VM. */
typedef struct {
	const char* source;
	const char* module;
	lt_Tokenizer tokens;
	lt_Parser parser;
	uint8_t is_valid;
} lt_Analysis;

lt_Tokenizer lt_tokenize(lt_VM* vm, const char* source, const char* mod_name);
lt_Parser lt_parse(lt_VM* vm, lt_Tokenizer* tkn);
lt_Value lt_compile(lt_VM* vm, lt_Parser* p);

/* Tokenizes and parses source without compiling or executing it. Any previous
   diagnostics on the VM are discarded. */
void lt_analysis_run(lt_VM* vm, lt_Analysis* analysis, const char* source, const char* module);
void lt_analysis_free(lt_VM* vm, lt_Analysis* analysis);

void lt_diagnostics_clear(lt_VM* vm);
uint32_t lt_diagnostic_count(lt_VM* vm);
const lt_Diagnostic* lt_diagnostic_get(lt_VM* vm, uint32_t index);

void lt_free_parser(lt_VM* vm, lt_Parser* p);
void lt_free_tokenizer(lt_VM* vm, lt_Tokenizer* tok);
