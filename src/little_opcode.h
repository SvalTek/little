#pragma once

#include "little.h"

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
	LT_OP_ITER,
	LT_OP_AITER, LT_OP_ABIND, LT_OP_ACLOSE,

	LT_OP_CLOSE, LT_OP_CALL, LT_OP_CALLM, LT_OP_FIXRET, LT_OP_PACKRET, LT_OP_SUPERC, LT_OP_SUPERM,

	LT_OP_MAKET, LT_OP_MAKEA, LT_OP_MAKEC, LT_OP_SETSUPER, LT_OP_SETT, LT_OP_SETC, LT_OP_GETT, LT_OP_GETD, LT_OP_GETG, LT_OP_SETG,

	LT_OP_JMP, LT_OP_JMPC, LT_OP_JMPN,

	/* Placeholder for `break`; the enclosing loop resolves it to jump past
	   itself. Kept distinct from JMP so a zero-distance jump emitted by any
	   other construct cannot be mistaken for an unresolved break. */
	LT_OP_BREAK,

	LT_OP_RET, LT_OP_RETM,
} lt_OpCode;

typedef struct {
	uint16_t op;
	int16_t arg;
} lt_Op;
