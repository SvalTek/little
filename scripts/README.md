# Little Scripts

These are runnable Little scripts that show the main language and stdlib features working together.
They are written as a commented tour, so reading the files should teach the
feature as well as prove that it runs.

Build the CLI first:

```powershell
task build
```

Run one example:

```powershell
build/little.exe scripts/01_language_basics.little
```

Run the whole gallery:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run.ps1
```

Tour order:

1. `01_language_basics.little` covers comments, named functions, function values, closures, globals, arrays, tables, receiver calls, table-call sugar, `with`, and loops.
2. `02_stdlib_collections.little` covers array, table, and string collection helpers.
3. `03_math_and_strings.little` covers math helpers, formatting, and string expansion.
4. `04_classes.little` covers constructors, private/public members, accessors, inheritance, `super`, and `override`.
5. `05_async_promises_tasks.little` covers promises, timers, async functions, global async functions, and tasks.
6. `06_io_files.little` covers file reads/writes and console output.
7. `07_destructuring_unpack.little` covers table destructuring, array destructuring, and `unpack`.
8. `08_task_real_work.little` covers a larger task state workflow.
9. `09_mainloop_scheduler.little` covers coordinating timers, tasks, and `mainloop.run`.
10. `10_modules.little` covers module returns, named imports, directory modules, search paths, and package path templates.
11. `11_conditions_operators.little` covers `if`, `elseif`, `else`, `while`, truthiness, `is`/`isnt`, logical operators, and operator precedence.

The file IO script writes to `build/example-note.txt`.
The module tutorial imports fixture modules from `scripts/tutorial_modules/` and
`scripts/tutorial_packages/`; those nested files are support material, not
standalone gallery entries.
