#include "little_std.h"
#include "little_loadlib.h"

lt_Value ltopen(lt_VM* vm, const lt_Api* api);
void lt_term_shutdown(void);
uint8_t lt_term_write_output(const char* text);
void lt_term_begin_composer(void);
void lt_term_update_composer(const char* text);
void lt_term_commit_composer(void);

/**
 * Opens the built-in terminal module and registers it as the global `term` module.
 * @param vm Virtual machine used to load and register the terminal module.
 */
void ltstd_open_term(lt_VM* vm)
{
    lt_Value term = ltopen(vm, ltstd_native_api());
    if (LT_IS_NULL(term)) lt_runtime_error(vm, "Built-in terminal module rejected the Little API!");
    lt_table_set(vm, vm->global, lt_make_string(vm, "term"), term);
    ltstd_set_output_writer(lt_term_write_output);
}

/**
 * Shuts down the terminal integration and clears the output writer.
 */
void ltstd_close_term(void)
{
    lt_term_shutdown();
    ltstd_set_output_writer(0);
}

/**
 * Begins a terminal composer session.
 */
void ltstd_term_begin_composer(void)
{
    lt_term_begin_composer();
}

/**
 * Updates the terminal composer with the specified text.
 *
 * @param text Text to apply to the composer.
 */
void ltstd_term_update_composer(const char* text)
{
    lt_term_update_composer(text);
}

/**
 * Commits the current terminal composer content.
 */
void ltstd_term_commit_composer(void)
{
    lt_term_commit_composer();
}
