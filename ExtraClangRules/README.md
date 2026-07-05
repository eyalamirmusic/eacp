# ExtraClangRules

Custom clang-tidy-style checks for the eacp repo rules (CLAUDE.md) that stock
clang-tidy can't express. Built on the libclang Python bindings; reads
`compile_commands.json` and prints clang-tidy-style diagnostics.

## Usage

```bash
# From this directory. Configure the build first so compile_commands.json
# exists (use -DEACP_UNITY_BUILD=OFF for per-file compile commands):
uv run main.py                      # checks Lib/ and Apps/
uv run main.py ../Lib/eacp/Core     # restrict to a subtree
uv run main.py -p ../build          # explicit build dir (default: ../build)
uv run main.py --checks eacp-use-auto,eacp-no-raw-new-delete
uv run main.py --checks '*,-eacp-no-body-comments'
uv run main.py --list-checks
```

Exit code is 1 when any warning is emitted, so it can gate CI.

## Auto-fix

```bash
uv run main.py --fix                # apply fixes repo-wide
uv run main.py --fix ../Lib/eacp/Core
```

`--fix` applies clang-tidy-style fix-its, then runs `clang-format` on just the
touched lines (picking up the repo `.clang-format`). Without `--fix`, fixable
warnings are marked `(has fix)`. What gets fixed:

- `eacp-use-auto`: the spelled-out type is replaced with `auto`, keeping
  `const`/`&`/`*` (`Widget* w = ...` → `auto* w = ...`). Only copy-init
  (`T x = ...`) and range-for variables are rewritten; paren-init like
  `T x(...)` is warn-only since the fix would need restructuring.
- `eacp-no-auto-function-return`: the deduced return type replaces `auto`
  (`auto area() { return 1.0; }` → `double area()`), and trailing return
  types are folded back (`auto f() -> int` → `int f()`). Function templates
  whose return stays dependent are warn-only.
- `eacp-std-function-member-default`: missing/null defaults become a no-op
  lambda matching the signature — `= [] {}`, or
  `= [] (const Args&) { return R {}; }` for non-void signatures.

`eacp-no-raw-new-delete` and `eacp-no-body-comments` are never auto-fixed —
those need a human (ownership redesign / extracting named functions).

## Checks

| Check | Repo rule |
| --- | --- |
| `eacp-use-auto` | "Use auto for variables and whenever possible." Flags a variable that spells out its type when the initializer already has exactly that type, so `auto` is a drop-in (init-list and array initializers are exempt, since `auto` would change the meaning). |
| `eacp-no-auto-function-return` | "Don't use auto for functions and member functions." Flags `auto`/`auto ... ->` return types on functions, methods, and function templates (lambdas exempt). |
| `eacp-std-function-member-default` | "Give std::function members a non-null default." Flags `std::function` data members (including via aliases like `Callback`) with no default or a null default (`{}`, `nullptr`). |
| `eacp-no-raw-new-delete` | "Always use the most modern C++ and RAII practices." Flags raw `new`/`delete` expressions. |
| `eacp-no-body-comments` | "Don't use comments unless absolutely needed. Use named functions to make code self documenting." Flags comments inside function bodies. |

## Suppressing a finding

Same syntax as clang-tidy, on the offending line or the one above:

```cpp
int status = 0; // NOLINT(eacp-use-auto) — out-param for waitpid
// NOLINTNEXTLINE(eacp-no-raw-new-delete)
auto* raw = new Widget();
```

## Notes

- Uses the Xcode toolchain's `libclang.dylib` when available (it matches the
  SDK headers); falls back to the pip-bundled libclang plus the system clang's
  `-resource-dir`. Override with `EACP_LIBCLANG=/path/to/libclang.dylib`.
- Headers are checked through every TU that includes them; duplicate findings
  are deduplicated, and anything outside the given paths (system headers,
  `build/`, `_deps/`) is ignored.
