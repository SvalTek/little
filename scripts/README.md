# Little Scripts

These are runnable Little scripts that show the main language and stdlib features working together.

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

The file IO script writes to `build/example-note.txt`.
