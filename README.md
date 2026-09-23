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
build/little.exe scripts/hello.little
build/little.exe -e 'io.print("hello")'
build/little.exe -I lib scripts/main.little
build/little.exe -L native scripts/main.little
```

The CLI accepts one script path, `-e SOURCE` for a short inline program, or
`-i` / `--interactive` for an editable terminal prompt. The prompt keeps its
in-process command history, reserves its `>> ` bottom line while output scrolls
above it, and supports `"""` to begin and end multiline input. Press Ctrl-C or
Ctrl-D to leave it. Use `--help` to see all options and `--version` to see the
linked API version. The process returns a non-zero exit code for command-line,
file, parse, or runtime errors.

Use `-I DIRECTORY` (more than once if needed) to add source-module search paths
and `-L DIRECTORY` (or `--library-path`) to add native-library search paths.
The script name and its following command-line arguments are available as the
global `arg` array: `arg[0]` is the script path and `arg[1...]` are supplied
arguments.

For persistent local paths, Little reads `~/.config/little.conf` and
`little.conf` next to the executable. Use `--config FILE` to select one file or
`--no-config` to ignore both defaults. The format is one setting per line:

```ini
# Relative paths are resolved from this config file.
module_path = projects/little-modules
library_path = ../lib/little
# Echo submitted input above its result in `little -i`.
repl_echo = true
```

Repeat `module_path` or `library_path` to register more paths. `repl_echo`
defaults to `false`.

For a local Windows compiler, copy `.env.example` to `.env` and set `GCC_PATH`
to the w64devkit root; the Taskfile loads that file without committing it.

### CI and release packages

GitHub Actions builds and tests on Windows and Linux. It downloads the
pinned WebUI submodule and compiles WebUI without launching a GUI. Pull requests
targeting `develop` produce per-PR development packages. Pushes to `develop`
produce development packages after merges; pushes to `main` produce optimized
release packages after merges from `develop`. The workflow can also be manually
run for any ref, defaulting to `develop`, with either package type.

Each run uploads one self-contained package for every target:

* `little-windows-x64.zip` contains `little.exe`, `libs/json/json.dll`, and
  `libs/webui/webui.dll`.
* `little-linux-x64.zip` contains `little`, `libs/json/json.so`, and
  `libs/webui/webui.so`.

The produced packages are portable: the executable is at the archive root and
native libraries are below `./libs`. The CLI also recognizes an installed
`bin` plus `lib/little` layout for manual deployments. Native packages may use
either `name/name.<platform-extension>` or the flat `name.<platform-extension>`
layout.

The supported build entry point is `build.ps1` (normally through `task build`).
It builds the vendored PDCursesMod backend and links it statically into the
CLI: WinCon on Windows and VT on Linux. This keeps the interactive CLI
self-contained.
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
