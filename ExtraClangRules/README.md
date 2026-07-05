# ExtraClangRules

Custom clang-tidy checks enforcing the eacp CLAUDE.md rules, implemented the
canonical way: an out-of-tree [clang-tidy check plugin](https://clang.llvm.org/extra/clang-tidy/Contributing.html)
(`plugin/`) that stock clang-tidy loads with `--load`. The checks then behave
exactly like built-in ones — configured through the repo `.clang-tidy`,
suppressed with `NOLINT(check-name)` / `NOLINTNEXTLINE(check-name)`, and
fixable with `-fix`.

## Checks

| Check | Repo rule |
| --- | --- |
| `eacp-use-auto` | "Use auto for variables and whenever possible." Flags a variable that spells out a type its initializer already has. Fix-it where dropping the type is a pure substitution (copy-init and range-for). Exempt: `= {...}` initializers (auto would deduce `initializer_list`) and `id` (deducing `id` is a clang warning). |
| `eacp-no-auto-function-return` | "Don't use auto for functions and member functions." Flags `auto` and trailing return types on functions, methods, and function templates; lambdas exempt. Fix-it substitutes the deduced type where clang has resolved it. |
| `eacp-std-function-member-default` | "Give std::function members a non-null default." Flags `std::function` data members (through aliases) with no default or a null default; the fix-it generates a signature-matching no-op lambda. Exempt: lambda captures and Objective-C ivars (often branched on for emptiness). |
| `eacp-no-raw-new-delete` | "Always use the most modern C++ and RAII practices." Flags raw `new`/`delete`. |
| `eacp-no-body-comments` | "Don't use comments unless absolutely needed. Use named functions to make code self documenting." Flags comments inside function/lambda bodies (comments containing NOLINT are ignored). |

## The pinned LLVM

Plugins have **no ABI stability guarantee** — the library must be built
against the exact clang-tidy that loads it. The team pin is **LLVM 21**:

```bash
brew install llvm@21          # macOS; CI uses the same formula
```

To move the pin, bump it here, in `bin/eacp-clang-tidy`, and in
`.github/workflows/build.yml`, then rebuild the plugin.

## Building the plugin

```bash
cmake -G Ninja -B ExtraClangRules/plugin/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm@21 \
      ExtraClangRules/plugin
cmake --build ExtraClangRules/plugin/build
```

## Running

`bin/eacp-clang-tidy` is stock clang-tidy from the pinned LLVM with the
plugin pre-loaded (`EACP_LLVM_PREFIX` overrides the LLVM location):

```bash
# One file (needs build/compile_commands.json: configure with
# -DEACP_UNITY_BUILD=OFF):
ExtraClangRules/bin/eacp-clang-tidy -p build --checks='-*,eacp-*' \
    Lib/eacp/Core/Threads/Timer.mm

# Apply fix-its:
ExtraClangRules/bin/eacp-clang-tidy -p build --checks='-*,eacp-*' -fix <file>

# Whole repo, in parallel:
/opt/homebrew/opt/llvm@21/bin/run-clang-tidy \
    -clang-tidy-binary "$PWD/ExtraClangRules/bin/eacp-clang-tidy" \
    -p build -checks='-*,eacp-*' \
    -header-filter='.*/(Lib|Apps)/.*' \
    -exclude-header-filter='.*/(build|_deps)/.*' \
    -quiet "$PWD/(Lib|Apps)/.*"
```

CI (`eacp-tidy` job in `.github/workflows/build.yml`) does exactly the
whole-repo run with `-warnings-as-errors='eacp-*'`, after first asserting
that `plugin/test/Sample.cpp` still trips all 18 expected findings.

## Editor setup

**CLion** — Settings → Languages & Frameworks → C/C++ → Clang-Tidy → set
the Clang-Tidy executable to `<repo>/ExtraClangRules/bin/eacp-clang-tidy`.
The eacp checks are already enabled in the repo `.clang-tidy`, so squiggles
appear natively. (The setting is IDE-wide, but the wrapper degrades to plain
pinned clang-tidy semantics elsewhere.)

**Neovim / VS Code (clangd)** — clangd cannot load clang-tidy plugins
([clangd#1458](https://github.com/clangd/clangd/issues/1458)); the other
`.clang-tidy` checks still come through clangd, and unknown `eacp-*` names
are ignored. For the eacp checks, run the wrapper as a linter:

- nvim, dependency-free: `dofile("<repo>/ExtraClangRules/nvim/eacp-tidy.lua").setup()`
  lints on open/save via `vim.diagnostic`.
- nvim-lint users: use the built-in `clangtidy` linter with
  `cmd = "<repo>/ExtraClangRules/bin/eacp-clang-tidy"`.
- VS Code: the clang-tidy extension accepts a custom executable path the
  same way.

## History

The first implementation was a Python checker on libclang bindings; it was
retired for this plugin once the plugin reached parity (the plugin actually
sees more: template bodies, ObjC ivars, lambda captures). The five rules were
applied repo-wide under the Python tool — roughly 250 auto conversions,
std::function defaults, and ~380 in-body comments relocated or extracted.
