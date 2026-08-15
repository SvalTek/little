# little - tiny bytecode language
little is a _small_, _fast_, _easily embeddable_ language implemented in C.

---
```js
var speak = fn(animal) {
    if animal is "cat" { return "meow" }
    elseif animal is "dog" { return "woof" }
    elseif animal is "mouse" { return "squeak" }
    return "???"
}

var animals = [ "cat", "dog", "mouse", "monkey" ]
for animal in array.each(animals) {
    io.print(string.format("%s says %s!", animal, speak(animal)))
}
```
---
## Feature Overview
* Tiny implementation - core langauge is <2500 sloc in a single .h/.c pair
* Light embedding - compiles down to less than 20kb, 3 API calls to get started
* Reasonably fast for realtime applications
* Low memory footprint with simple mark and sweep garbage collector
* Supports null, numbers, booleans, strings, functions, closures, arrays, tables, and native procedures
* Optional, consise stdlib - an extra ~1000 sloc
* Supports 32- and 64-bit, and will likely compile anywhere!
* Feature-rich C api to integrate and interact with the VM
---
## Simple embedding example
```c
#include "little.h"
#include "little_std.h"
#include "little_async.h"

// this is called if the vm encounters an error, letting us react
void my_error_callback(lt_VM* vm, const char* msg)
{
    printf("LT ERROR: %s\n", msg);
}

int main(char** argv, int argc)
{
    lt_VM* vm = lt_open(malloc, free, my_error_callback);                    // open new VM
    ltstd_open_all(vm);                                                      // register stdlib
    ltasync_open_all(vm);                                                    // optional Promise, timer, and task libs

    const char* my_source_code = ...                                         // read source from file/stream/string

    uint16_t n_return = lt_dostring(vm, my_source_code, "my_module")         // run code as "my_module" 
    if(n_return) printf("LT RETURNED: %s", ltstd_tostring(vm, lt_pop(vm)));  // if our code returns, print the result
}
```
---
## Compiling

The included Taskfile is the supported development workflow:

```powershell
task build
build/little.exe --help
build/little.exe -i
build/little.exe scripts/hello.little first-argument
build/little.exe -e 'io.print("hello")'
build/little.exe -I lib scripts/main.little
build/little.exe -L native scripts/main.little
```

The CLI accepts one script path, `-e SOURCE` for a short inline program, or
`-i` / `--interactive` for an editable interactive prompt. The prompt keeps
its in-process command history and reserves its `>> ` bottom line while output
scrolls above it. Enter `"""` to start a multiline capture; entering `"""`
again submits and runs the captured chunk. Capture lines grow upward from the
bottom while they are being edited, then clear before the output is shown.
Press Ctrl-C or Ctrl-D to leave it. It
returns a non-zero exit code for command-line, file, parse, or runtime errors.
Use `--help` to see its options and `--version` to see the linked API version.
Use `-I DIRECTORY` (more than once if needed) to add source-module search paths
without baking those local launch details into the script.

Use `-L DIRECTORY` (or `--library-path DIRECTORY`) to add a native-library
search path for `loadLibrary(...)`. The script name and its following command
line arguments are available as the global `arg` array: `arg[0]` is the script
path and `arg[1...]` are the supplied arguments.

For persistent local paths, Little reads `~/.config/little.conf` and
`little.conf` next to the executable. Use `--config FILE` to select one file or
`--no-config` to ignore both defaults. The format is one setting per line:

```ini
# Relative paths are resolved from this config file.
module_path = projects/little-modules
library_path = ../lib/little
```

Only `module_path` and `library_path` are currently supported; repeat either
setting to register more paths. Unknown settings are errors, so a misspelled
path setting cannot silently change program behavior.

For a local Windows compiler, copy `.env.example` to `.env` and set `GCC_PATH`
to the w64devkit root; the Taskfile loads that file without committing it.

### CI and release packages

GitHub Actions builds and tests on Windows and Linux. It downloads the
pinned WebUI submodule and compiles WebUI without launching a GUI. Pull requests
targeting `develop` produce per-PR development packages. Pushes to `develop`
produce development packages after merges; pushes to `main` produce optimized
release packages after merges from `develop`. The workflow can also be manually
run for any ref, defaulting to `develop`, with either package type.

Each successful run attaches one self-contained package for every target to a
rolling GitHub Release: `pr-<number>` for pull requests, `develop` for develop
builds, and `main` for main builds.

* `little-windows-x64.zip` contains `bin/little.exe`, `lib/little/json/json.dll`,
  and `lib/little/webui/webui.dll`.
* `little-linux-x64.zip` contains `bin/little`, `lib/little/json/json.so`, and
  `lib/little/webui/webui.so`.

Extract a package into an install prefix (for example `~/.local`) so the
executable lives in `bin` and its libraries live in `lib/little`. The CLI
searches that installed native-library directory automatically. It also
recognizes the older portable `libs/` directory beside the executable. A
packaged library can be loaded by name, for example `loadLibrary("json")` or
`loadLibrary("webui")`: the loader tries both `name.*` and `name/name.*`.

The supported build entry point is `build.ps1` (normally through `task build`).
It builds the vendored PDCursesMod backend and links it statically into the
CLI: WinCon on Windows and the VT backend on Linux. This keeps the interactive
CLI self-contained and avoids a terminal-library DLL beside `little.exe`.
---
## Links
* **[Language overview](doc/lt.md)**
* **[Syntax rules](doc/syntax.md)**
* **[Runtime and compiler limits](doc/limits.md)**
* **[Standard library](doc/ltstd.md)**
* **[Task API](doc/std/task.md)**
* **[C API reference](doc/api.md)**
* **[C API examples](doc/example.md)**
* **[Runnable scripts](scripts/README.md)**
---
## Contribution
Feel free to open an issue or pull request if you feel you have something meaninfgul to add, but keep in mind the language is minimalist by design, so any merging will be very carefully picked

---
## License
Please see [LICENSE](LICENSE) for details
