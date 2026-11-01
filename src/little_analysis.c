#include "little_vm_layout.h"
#include "little_frontend.h"

#include <string.h>

/* Analysis runs the front end without compiling or executing anything, and
   keeps the token stream and syntax tree alive for tooling. */
void lt_analysis_run(lt_VM* vm, lt_Analysis* analysis, const char* source, const char* module)
{
	memset(analysis, 0, sizeof(lt_Analysis));
	analysis->source = source;
	analysis->module = module;

	lt_diagnostics_clear(vm);
	vm->collect_diagnostics = 1;

	analysis->tokens = lt_tokenize(vm, source, module);
	if (analysis->tokens.is_valid)
	{
		analysis->parser = lt_parse(vm, &analysis->tokens);
		analysis->is_valid = analysis->parser.is_valid;
	}

	vm->collect_diagnostics = 0;
}

void lt_analysis_free(lt_VM* vm, lt_Analysis* analysis)
{
	lt_free_parser(vm, &analysis->parser);
	lt_free_tokenizer(vm, &analysis->tokens);
	memset(analysis, 0, sizeof(lt_Analysis));
}
