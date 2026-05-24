# AGENTS.md

Guidance for LLM agents working on `little`.

## Project Intent

`little` is a tiny embeddable bytecode language written in C. Keep changes small,
explicit, and in sympathy with the existing design. Prefer predictable limits,
clear runtime errors, and direct C over broad abstractions.

The project is intentionally minimalist. Do not add large frameworks, generated
systems, or sweeping rewrites unless the user explicitly asks for that direction.

## Repository Map

- `src/little.c` and `src/little.h` are the core language: tokenizer, parser,
  compiler, VM, GC, values, objects, tables, arrays, and public API.
- `src/little_buffer.c` is the shared growable-buffer implementation.
- `src/little_internal.h` is for private cross-file declarations only.
- `src/little_std*.c` and `src/little_std.h` hold standard-library modules.
- `src/little_async.c` and `src/little_async.h` hold optional async-related
  runtime support. Avoid encoding assumptions about this area unless directly
  working on it; it is expected to evolve.
- `doc/` mirrors user-facing behavior. Language docs live in `doc/lt.md`,
  C API docs in `doc/api.md`, limits in `doc/limits.md`, and stdlib docs under
  `doc/std/`.
- `scripts/` contains runnable Little examples and should stay aligned with
  documented features.
- `tests/e2e/` contains feature and behavior tests.
- `tests/fuzz/` contains regression, crash, parser-edge, runtime-recovery, and
  semantic corner-case tests.

## Coding Conventions

- Keep public API declarations in `src/little.h`, stdlib declarations in
  `src/little_std.h`, and private shared declarations in `src/little_internal.h`.
- Do not introduce externally visible C symbols with reserved leading-underscore
  names.
- Prefer `static` helpers for file-local implementation details.
- Use the existing stack/native calling style: validate arity, pop arguments in
  stack order, type-check explicitly, push return values, and return the number
  of pushed values.
- Use specific error messages. Tests often assert exact or contained diagnostic
  text.
- For Little runtime errors, prefer `lt_runtime_error`. Parser/tokenizer/compiler
  errors should use the existing parse/tokenize error paths so module and
  location formatting remains consistent.
- When adding a new object type, update equality, GC marking, freeing, string
  conversion if applicable, and any relevant type macros.
- When adding references between heap objects, update GC traversal. The collector
  must tolerate cycles.
- Keep fixed-size limits guarded and documented. Overflows should fail cleanly,
  not corrupt memory.

## Reuse And Refactoring

Do not duplicate logic when the project already has a shared concept, helper, or
pattern that fits. Prefer using the existing abstraction over copying a similar
implementation into another module.

If a change needs behavior that already exists elsewhere but is not reusable in
its current form, consider extracting the shared piece into the narrowest
appropriate helper or internal API. Keep the extraction modest: it should remove
real duplication, preserve local clarity, and match the existing ownership
boundaries.

Do not introduce a broad abstraction just because two blocks look similar. Share
logic when the behavior is genuinely the same and likely to stay coupled; keep
code local when the similarity is accidental or the call sites have different
semantics.

## Documentation Rules

User-facing behavior changes should update docs in the same change.

- Language syntax or semantics: update `doc/lt.md`.
- Class behavior: update `doc/classes.md`.
- C API behavior: update `doc/api.md`.
- Runtime/compiler limits: update `doc/limits.md`.
- Stdlib behavior: update `doc/ltstd.md` and the relevant `doc/std/*.md`.
- Runnable example changes: update `scripts/README.md` if usage changes.

Markdown tables should be surrounded by blank lines.

## Test Rules

Every `.little` test must have one assertion companion:

- `.expected` for exact normalized output.
- `.contains` for diagnostic text where full output is intentionally brittle.

The test runner treats missing companions as failures.

Use `tests/e2e/` for normal user-visible behavior. Use `tests/fuzz/` for
regressions, crash repros, parser edge cases, runtime recovery, limits, and
semantic corner cases.

Negative tests should isolate one intended failure when practical. Runtime
recovery tests should make success obvious in the expected output, for example
by printing booleans that are `true` when the expected error occurred.

## Validation

Before handing work back, run:

```powershell
git diff --check
.\tests\run-e2e.ps1
```

Use `task test` when Task is available. Build output belongs in `build/`.

## Change Discipline

- Read the surrounding code before editing; match local patterns.
- Keep edits scoped to the requested behavior.
- Do not silently reformat unrelated files.
- Do not update docs without tests when behavior changes, and do not add tests
  that merely check that a command ran without asserting behavior.
- If a review comment is no longer valid, say why instead of forcing a needless
  change.
- If behavior is intentionally not covered because it is volatile or being
  redesigned, avoid encoding it as a durable project rule.
- Investigate existing behavior and nearby patterns before changing design,
  semantics, public API, tests, or docs.
- Ground changes in the project intent and conventions. If intent is unclear and
  the choice would affect user-visible behavior or long-term structure, ask for
  guidance before proceeding.
