# C implementation architecture

The implementation lives in the flat `src/` directory. `little.h` remains the
compatibility umbrella for embedders; it includes the value, frontend, API and
VM layout headers. `little_vm_layout.h` is the advanced layout interface for
code that accesses VM fields. The value header keeps object layouts available.

The frontend is VM-based. `little_lexer.c`, `little_parser.c`, and
`little_analysis.c` provide tokenization, parsing and analysis through
`little_frontend.h`. Compilation is in `little_compiler.c`; the interpreter,
VM lifecycle and execution entry points are in `little.c`. Runtime values,
tables, classes, errors, garbage collection and polling have their own source
files. The compiler and VM share internal opcode definitions.

The core does not require the standard library. Standard-library registration,
module loading and conversion live in `little_std*.c`. Promise execution,
polling and worker lifecycle state live in the core; optional async registration
and worker startup live in `little_async_std.c` and `little_task.c`. Shared
values used by workers live in `little_shared.c`.
The CLI and bundle builder are separate consumers. Socket primitives shared by
the web and WebSocket modules live in `little_net.c`.

Native plugins use the version 3 `lt_Api` table from `little_api.c` and the
loader in `little_loadlib.c`. `little_std.h` does not include the loader header;
code using the loader includes `little_loadlib.h` explicitly. Runtime bundle
reading is declared in `little_bundle.h`; bundle creation is declared in
`little_bundle_build.h`.

`scripts/sources.ps1` defines the source sets used by the CLI, native tests and
language server. `task build` builds the CLI, `task build:lib` creates
`build/liblittle.a`, and `task check:core` checks standalone public headers and
links and runs a core-only C program. `task test` runs the project tests and
the core checks. Build products belong in `build/`; use the gitignored `temp/`
directory for scratch experiments, generated helpers and logs.
