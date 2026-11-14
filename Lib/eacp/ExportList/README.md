# ExportList

`eacp-export-list-tool` writes the module-definition file (`.def`) that lists
what a Windows executable exports to its hot-reload module, read from the COFF
objects and static archives the executable links. It replaces CMake's
undocumented `cmake -E __create_def`, applies a filter that keeps the list well
under the 65,535 names a PE image can export, and refuses any input it cannot
read rather than writing a short list. Windows only; it is built wherever
`WIN32` is and the build is not cross-compiling.

On Windows a module cannot bind to the executable's symbols at load time the
way it does on macOS and Linux. Instead the executable is linked with `/DEF:`
the list, an import library is made from the list alone, and the module links
that import library and nothing of eacp, so rebuilding it never touches the
running, locked executable. This directory is the list;
`eacp_add_hot_reload` (`CMake/EacpPlugin.cmake`) wires it into every Windows
dev-mode app, as "Windows" in `Docs/HotReload.md` describes.

## Use

```cmake
eacp_add_export_list(<name>
    DEF <file>                 # the .def to write
    IMAGE <file name>          # the image the exports live in, e.g. MyApp.exe
    [TARGETS <target>...]      # static libraries are read as their archives;
                               # executables, shared and object libraries as
                               # their objects
    [OBJECTS <file>...]        # further objects or archives
    [IMPORT_LIBRARY <file>]    # made from DEF alone, with $CMAKE_AR /def:
    [UNFILTERED])              # __create_def's rules only, to compare
```

`CMake/EacpExportList.cmake` adds the custom target `<name>`. The tool itself:

```
eacp-export-list-tool --output <def> --name <App.exe> [--inputs <list>]...
                      [<object or archive>...] [--unfiltered]
                      [--warn-at <n>] [--limit <n>] [--report <file>]
```

`--inputs` is a file of paths, one a line. The `.def` starts with
`NAME <App.exe>`, because `llvm-lib` ignores `/name:` and would otherwise
write imports from no DLL, then `EXPORTS`, one name a line, sorted, with data
tagged `DATA`. `--report` writes every defined name with its verdict, kind,
COMDAT flag and undecorated form, for inspecting a filter.

The file is rewritten only when its content changes, and Ninja's custom
commands are `restat`, so an edit that changes no export reruns the tool and
nothing after it: the import library is not remade and the module does not
relink. On any refusal the tool deletes the old list, so nothing links against
a stale one.

## What is listed

Every external symbol an input defines in one of its own sections, less:

| Dropped | Why |
| --- | --- |
| Deleting destructors (`??_G`, `??_E`), names with a `.`, C++/CLI thunks, operator-only C names | `__create_def` drops them; the compiler regenerates them |
| Read-only data: string literals (`??_C@`), `__real@`/`__xmm@` constants, RTTI | Constants each image has its own copy of |
| COMDAT data: inline variables, static members of templates, vftables, guard variables | The module compiles its own copy, and data cannot be imported without `__declspec(dllimport)` anyway |
| COMDAT functions of namespace `std` | The module instantiates its own; 60 % of the unfiltered list |

Kept: every non-COMDAT definition, every COMDAT function outside `std`, and
writable data, tagged `DATA`. Explicit template instantiations are COMDATs
under MSVC, so a "non-COMDAT only" rule would drop names modules really import
(the UI example's module imports `Rectangle<float>::inset`, an explicit
instantiation). A COMDAT here is one the linker picks a single copy of from any
number of objects; `/Gy`'s and `/Gw`'s no-duplicates COMDATs are ordinary
definitions in a section of their own, so an out-of-line explicit
specialisation such as `std::hash<Key>::operator()` is kept in every
configuration.

`std` is the outermost scope as dbghelp's `UnDecorateSymbolName` reads it
(`UNDNAME_NAME_ONLY`), which is called serially, since it is not thread-safe,
and only for names containing `std@@`: a name whose outermost scope is `std`
always spells that out, because a back-reference can only repeat a scope already
spelled at that level and template arguments keep a table of their own. On
five real closures (MSVC Debug and RelWithDebInfo, clang-cl Debug) the
undecorator failed on no name, and undecorating every name instead of the
prefiltered ones changed no verdict.

The undecorator is the `dbghelp.dll` of the machine the build runs on, so the
same inputs can give a different list on a machine whose dbghelp cannot
undecorate a newer mangling. A name it fails on (or one longer than its
4,095-character buffer) is not recognised as `std` and is kept: the list
grows, and a module never loses a symbol it needs.

What a module cannot do with this list: rely on an `extern template`
declaration of a `std` specialisation, or share header-defined state with the
executable (each image has its own copy, as it always has on Windows).

## Refusals

The build fails, naming the file (`archive.lib(member.obj)` inside an archive),
for an object compiled with link-time optimisation — MSVC's `/GL` output or
LLVM bitcode from clang-cl's `-flto` — since neither carries a COFF symbol
table; link-time optimisation must be off for everything in the closure. It
fails likewise on a corrupt or unknown input, a thin archive member that is
missing or changed size, inputs for two machines, no inputs, or an empty list.

## The limit

A PE image exports at most 65,535 names. The tool prints the count on every
run, warns at 75 % (49,149) and fails above 65,535. With the filter the UI
example is at about 21 %; unfiltered it would be at 56 %.

## Formats

Classic and `/bigobj` COFF objects, from MSVC `cl` and clang-cl, for x64 and
ARM64 (clang-cl accepts `/bigobj` but writes the extended header only for an
object that needs it, so its `/bigobj` output is untested); archives from MSVC `lib` and from `llvm-lib`, long member names and
`llvm-lib /llvmlibthin` thin archives included; short import objects inside an
archive are skipped. Other machines are refused.

## Measurements

The two hot-reload examples' closures, read from the same objects
`__create_def` read (or from their archives, which gives the same list):

| Closure | `__create_def` | Unfiltered | Filtered |
| --- | --- | --- | --- |
| UI example, MSVC Debug | 36,926 | 36,926 | 13,489 |
| UI example, MSVC RelWithDebInfo | 37,894 | 37,894 | 13,509 |
| UI example, clang-cl Debug | 34,634 | 34,634 | 13,225 |
| GPU example, MSVC Debug | 29,290 | 29,290 | 11,200 |
| GPU example, clang-cl Debug | 27,482 | 27,482 | 10,940 |

Unfiltered, the names are exactly `__create_def`'s; the only difference is that
vftables are tagged `DATA`, which they are, where `__create_def` lists them as
code. On the UI closure (279 objects, warm cache) `__create_def` takes about
90 ms, the tool about 100 ms in a Release build directory and about 175 ms in a
Debug one; it is force-optimised in every configuration but links the Debug
`eacp-core` and its checked STL there.

Every filtered list above was checked by linking: the example's real module
against an import library made from the list alone by `lib` and by `llvm-lib`,
with `link` or `lld-link`, and the executable with `/DEF:` the list. Whole test
suites (`UITests`, `GPUTests`, `CoreTests`, `TextTests`, `GPUWidgetsTests`)
were linked the same way as stand-in modules. None left a symbol unresolved.

## Layout

- `Coff.{h,cpp}` — object formats and the symbol table walk
- `Archive.{h,cpp}` — archive members, thin archives included
- `ExportList.{h,cpp}` — the collector, the filter, the `.def`, the limit
- `NameIndex.h` — the name table, flat so a Debug build stays fast
- `Undecorate-Windows.cpp` — dbghelp
- `Main.cpp` — the tool
- `Tests/ExportList` — the suite, which compiles its fixtures with the build's
  own compiler and librarian
