"""clang-tidy-style checks for the eacp repo rules (CLAUDE.md), built on libclang.

Usage:
    uv run main.py [paths...] [-p BUILD_DIR] [--checks a,b] [-j N]

Reads compile_commands.json, parses each TU with libclang, and emits
clang-tidy-style diagnostics:  file:line:col: warning: message [check-name]

Suppress a finding with // NOLINT or // NOLINT(check-name) on the same or
previous line.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import functools
import json
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from clang.cindex import (
    Config,
    Cursor,
    CursorKind,
    Index,
    SourceLocation,
    Token,
    TokenKind,
    TranslationUnit,
)

REPO_ROOT = Path(__file__).resolve().parent.parent


@functools.cache
def configure_libclang() -> bool:
    """Prefer the Xcode toolchain's libclang — it matches the SDK headers,
    which the pip-bundled libclang can be too old to parse. Returns True if a
    toolchain library was selected (it then finds its own builtin headers)."""
    candidates = [os.environ.get("EACP_LIBCLANG")]
    try:
        dev = subprocess.run(
            ["xcode-select", "-p"], capture_output=True, text=True
        ).stdout.strip()
        if dev:
            candidates.append(
                f"{dev}/Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib"
            )
    except OSError:
        pass
    for lib in filter(None, candidates):
        if Path(lib).exists():
            Config.set_library_file(lib)
            return True
    return False

ALL_CHECKS = {
    "eacp-use-auto": "use auto for variables whenever possible",
    "eacp-no-auto-function-return": "don't use auto return types on functions",
    "eacp-std-function-member-default": "std::function members need a non-null default",
    "eacp-no-raw-new-delete": "prefer RAII over raw new/delete",
    "eacp-no-body-comments": "avoid comments; use named functions instead",
}


Edit = tuple[int, int, str]  # byte offsets [start, end) -> replacement


class AddFn(Protocol):
    def __call__(
        self,
        cursor: Cursor,
        check: str,
        message: str,
        fix: tuple[Edit, ...] | None = None,
    ) -> None: ...


class AddAtFn(Protocol):
    def __call__(
        self,
        location: SourceLocation,
        check: str,
        message: str,
        fix: tuple[Edit, ...] | None = None,
    ) -> None: ...


@dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int
    column: int
    check: str
    message: str
    fix: tuple[Edit, ...] | None = None

    def render(self) -> str:
        rel = os.path.relpath(self.file, REPO_ROOT)
        return f"{rel}:{self.line}:{self.column}: warning: {self.message} [{self.check}]"


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------


@functools.cache
def real(path: str) -> str:
    return os.path.realpath(path)


def in_scope(path: str | None, scope_prefixes: tuple[str, ...]) -> bool:
    if not path:
        return False
    path = real(path)
    if "/build/" in path or "/_deps/" in path:
        return False
    return path.startswith(scope_prefixes)


def tokens_before_name(cursor: Cursor) -> list[Token]:
    """Tokens of a declaration up to its identifier, skipping template<...>
    parameter lists."""
    toks: list[Token] = []
    depth = 0
    saw_template = False
    for tok in cursor.get_tokens():
        s = tok.spelling
        if depth:
            depth += {"<": 1, ">": -1, ">>": -2}.get(s, 0)
            if depth <= 0:
                depth = 0
                saw_template = False
            continue
        if saw_template and s == "<":
            depth = 1
            continue
        if s == "template":
            saw_template = True
            continue
        if s == cursor.spelling or s == "(":
            return toks
        toks.append(tok)
    return toks


def tokens_after_name(cursor: Cursor) -> list[Token]:
    toks = list(cursor.get_tokens())
    names = [i for i, t in enumerate(toks) if t.spelling == cursor.spelling]
    return toks[names[0] + 1 :] if names else []


def normalized_type(spelling: str) -> str:
    spelling = re.sub(r"\b(const|volatile)\b", "", spelling)
    return re.sub(r"[&\s]+", "", spelling)


def first_init_expr(cursor: Cursor) -> Cursor | None:
    for child in reversed(list(cursor.get_children())):
        if child.kind.is_expression():
            return unwrap_implicit(child)
    return None


def has_explicit_initializer(cursor: Cursor) -> bool:
    """Distinguish 'Foo f = ...;' from 'Foo f;', where libclang still reports
    the implicit default-constructor call as an expression child."""
    toks = [t.spelling for t in cursor.get_tokens()]
    if cursor.spelling not in toks:
        return False
    after = toks[toks.index(cursor.spelling) + 1 :]
    return any(s in ("=", "{", "(") for s in after)


def unwrap_implicit(expr: Cursor) -> Cursor:
    """Skip ImplicitCastExpr wrappers (surfaced as UNEXPOSED_EXPR) so the
    initializer's pre-conversion type is compared."""
    while expr.kind == CursorKind.UNEXPOSED_EXPR:
        children = [c for c in expr.get_children() if c.kind.is_expression()]
        if len(children) != 1:
            break
        expr = children[0]
    return expr


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------


def check_use_auto(cursor: Cursor, parent: Cursor | None, add: AddFn) -> None:
    if cursor.kind != CursorKind.VAR_DECL:
        return
    if parent is not None and parent.kind == CursorKind.CXX_CATCH_STMT:
        return
    in_range_for = (
        parent is not None and parent.kind == CursorKind.CXX_FOR_RANGE_STMT
    )
    init = first_init_expr(cursor)
    if init is None or not (in_range_for or has_explicit_initializer(cursor)):
        return
    if init.kind == CursorKind.INIT_LIST_EXPR:
        return

    declared = cursor.type.get_canonical().spelling
    deduced = init.type.get_canonical().spelling
    if not declared or not deduced or "type-parameter" in declared:
        return
    if normalized_type(declared) != normalized_type(deduced):
        return

    before = tokens_before_name(cursor)
    spellings = [t.spelling for t in before]
    if not spellings or "auto" in spellings or "decltype" in spellings:
        return
    add(
        cursor,
        "eacp-use-auto",
        f"variable '{cursor.spelling}' spells out its type; use auto "
        "(CLAUDE.md: use auto for variables whenever possible)",
        fix=use_auto_fix(cursor, parent, before, in_range_for),
    )


NON_TYPE_TOKENS = {
    "const",
    "volatile",
    "static",
    "constexpr",
    "constinit",
    "inline",
    "mutable",
    "thread_local",
    "extern",
    "*",
    "&",
    "&&",
}


def use_auto_fix(
    cursor: Cursor,
    parent: Cursor | None,
    before: list[Token],
    in_range_for: bool,
) -> tuple[Edit, ...] | None:
    after = tokens_after_name(cursor)
    copy_init = bool(after) and after[0].spelling == "="
    if not (in_range_for or copy_init):
        return None
    if copy_init and len(after) > 1 and after[1].spelling == "{":
        return None  # '= {...}' would deduce initializer_list under auto
    if parent is not None and parent.kind == CursorKind.DECL_STMT:
        siblings = [
            c for c in parent.get_children() if c.kind == CursorKind.VAR_DECL
        ]
        if len(siblings) > 1:
            return None
    type_toks = [t for t in before if t.spelling not in NON_TYPE_TOKENS]
    if not type_toks:
        return None
    start = type_toks[0].extent.start.offset
    end = type_toks[-1].extent.end.offset
    return ((start, end, "auto"),)


def check_no_auto_return(
    cursor: Cursor, parent: Cursor | None, add: AddFn
) -> None:
    if cursor.kind not in (
        CursorKind.FUNCTION_DECL,
        CursorKind.CXX_METHOD,
        CursorKind.FUNCTION_TEMPLATE,
    ):
        return
    before = tokens_before_name(cursor)
    auto_tok = next((t for t in before if t.spelling == "auto"), None)
    if auto_tok is None:
        return
    add(
        cursor,
        "eacp-no-auto-function-return",
        f"function '{cursor.spelling}' uses an auto return type; spell out "
        "the return type (CLAUDE.md: don't use auto for functions)",
        fix=auto_return_fix(cursor, auto_tok),
    )


def auto_return_fix(cursor: Cursor, auto_tok: Token) -> tuple[Edit, ...] | None:
    auto_span = (auto_tok.extent.start.offset, auto_tok.extent.end.offset)

    trailing = trailing_return_tokens(cursor)
    if trailing is not None:
        arrow, type_toks = trailing
        if not type_toks:
            return None
        text = " ".join(t.spelling for t in type_toks)
        return (
            (*auto_span, text),
            (arrow.extent.start.offset, type_toks[-1].extent.end.offset, ""),
        )

    if cursor.kind == CursorKind.FUNCTION_TEMPLATE:
        return None

    deduced = cursor.result_type.spelling

    if not deduced or re.search(r"\bauto\b|\bdecltype\b|\(lambda|\(unnamed", deduced):
        return None

    return ((*auto_span, deduced),)


def trailing_return_tokens(
    cursor: Cursor,
) -> tuple[Token, list[Token]] | None:
    """The '->' token and the return-type tokens after it, or None."""
    toks = list(cursor.get_tokens())
    arrow = None
    depth = 0
    for i, tok in enumerate(toks):
        s = tok.spelling
        depth += {"(": 1, ")": -1}.get(s, 0)
        if s == "->" and depth == 0:
            arrow = i
            break
        if s == "{" and depth == 0:
            return None
    if arrow is None:
        return None
    type_toks: list[Token] = []
    for tok in toks[arrow + 1 :]:
        if tok.kind == TokenKind.COMMENT or tok.spelling in (
            "{",
            ";",
            "override",
            "final",
        ):
            break
        type_toks.append(tok)
    return toks[arrow], type_toks


NULL_DEFAULTS = {"{}", "={}", "=nullptr", "{nullptr}", "=NULL", "{NULL}"}


def check_std_function_member(
    cursor: Cursor, parent: Cursor | None, add: AddFn
) -> None:
    if cursor.kind != CursorKind.FIELD_DECL:
        return
    canonical = cursor.type.get_canonical().spelling
    if not canonical.removeprefix("const ").startswith("std::function<"):
        return

    after = tokens_after_name(cursor)
    initializer = "".join(t.spelling for t in after).rstrip(";")

    if not initializer:
        name_end = next(
            (
                t.extent.end.offset
                for t in cursor.get_tokens()
                if t.spelling == cursor.spelling
            ),
            None,
        )
        fix = None
        if name_end is not None and (lam := noop_lambda(cursor)):
            fix = ((name_end, name_end, f" = {lam}"),)
        add(
            cursor,
            "eacp-std-function-member-default",
            f"std::function member '{cursor.spelling}' has no default; give it "
            "a no-op lambda so call sites don't need null checks",
            fix=fix,
        )
    elif initializer in NULL_DEFAULTS:
        init_toks = [t for t in after if t.spelling != ";"]
        fix = None
        if init_toks and (lam := noop_lambda(cursor)):
            fix = (
                (
                    init_toks[0].extent.start.offset,
                    init_toks[-1].extent.end.offset,
                    f"= {lam}",
                ),
            )
        add(
            cursor,
            "eacp-std-function-member-default",
            f"std::function member '{cursor.spelling}' defaults to null; use a "
            "no-op lambda (e.g. [] {}) so call sites don't need null checks",
            fix=fix,
        )


def noop_lambda(cursor: Cursor) -> str | None:
    """A no-op lambda matching the std::function's signature, in repo style:
    '[] {}' or '[] (Args) { return R {}; }'."""
    for t in (cursor.type, cursor.type.get_canonical()):
        spelling = t.spelling
        if "function<" in spelling:
            sig = extract_signature(spelling)
            if sig:
                ret, params = sig
                if ret == "void":
                    capture = "[]" if not params else f"[] ({params})"
                    return f"{capture} {{}}"
                base = ret.split("<", 1)[0].strip()
                if " " in base:  # multiword builtin: no functional cast
                    return f"[] ({params}) -> {ret} {{ return {{}}; }}"
                capture = "[]" if not params else f"[] ({params})"
                return f"{capture} {{ return {ret} {{}}; }}"
    return None


def extract_signature(spelling: str) -> tuple[str, str] | None:
    inner = spelling[spelling.index("function<") + len("function<") :]
    inner = inner[: inner.rindex(">")].strip()
    if not inner.endswith(")"):
        return None
    depth = 0
    for i in range(len(inner) - 1, -1, -1):
        depth += {")": 1, "(": -1}[inner[i]] if inner[i] in "()" else 0
        if depth == 0:
            return inner[:i].strip(), inner[i + 1 : -1].strip()
    return None


def check_raw_new_delete(
    cursor: Cursor, parent: Cursor | None, add: AddFn
) -> None:
    if cursor.kind == CursorKind.CXX_NEW_EXPR:
        add(
            cursor,
            "eacp-no-raw-new-delete",
            "raw 'new'; prefer RAII (std::make_unique, containers, or an "
            "owning wrapper)",
        )
    elif cursor.kind == CursorKind.CXX_DELETE_EXPR:
        add(
            cursor,
            "eacp-no-raw-new-delete",
            "raw 'delete'; prefer RAII ownership so cleanup is automatic",
        )


def check_body_comments(
    cursor: Cursor, tu: TranslationUnit, add_at: AddAtFn
) -> None:
    if cursor.kind not in (
        CursorKind.FUNCTION_DECL,
        CursorKind.CXX_METHOD,
        CursorKind.CONSTRUCTOR,
        CursorKind.DESTRUCTOR,
        CursorKind.LAMBDA_EXPR,
    ):
        return
    body = next(
        (c for c in cursor.get_children() if c.kind == CursorKind.COMPOUND_STMT),
        None,
    )
    if body is None:
        return
    for tok in tu.get_tokens(extent=body.extent):
        if tok.kind == TokenKind.COMMENT and "NOLINT" not in tok.spelling:
            add_at(
                tok.location,
                "eacp-no-body-comments",
                "comment inside a function body; prefer a named function that "
                "makes the code self-documenting (CLAUDE.md)",
            )


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------


@functools.cache
def system_resource_dir() -> str | None:
    """The bundled libclang ships without builtin headers (stdarg.h etc.);
    borrow the system clang's resource directory."""
    for cmd in (["xcrun", "clang"], ["clang"]):
        try:
            out = subprocess.run(
                cmd + ["-print-resource-dir"], capture_output=True, text=True
            )
        except OSError:
            continue
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    return None


def build_clang_args(entry: dict[str, Any]) -> list[str]:
    raw: list[str] = entry.get("arguments") or shlex.split(entry["command"])
    args: list[str] = []
    skip_next = False
    for arg in raw[1:]:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-c", "--"):
            continue
        if arg in ("-o", "-index-store-path", "-MF", "-MT", "-MQ"):
            skip_next = True
            continue
        resolved = os.path.normpath(os.path.join(entry["directory"], arg))
        if resolved == os.path.normpath(entry["file"]):
            continue
        args.append(arg)
    args.append("-Wno-everything")
    needs_resource_dir = not configure_libclang()
    if (
        needs_resource_dir
        and "-resource-dir" not in " ".join(args)
        and (rd := system_resource_dir())
    ):
        args.append(f"-resource-dir={rd}")
    return args


def analyze(
    entry: dict[str, Any], scope_prefixes: tuple[str, ...], checks: set[str]
) -> tuple[list[Diagnostic], str | None]:
    diags: set[Diagnostic] = set()
    configure_libclang()
    index = Index.create()
    try:
        tu = index.parse(
            entry["file"],
            args=build_clang_args(entry),
            options=TranslationUnit.PARSE_INCOMPLETE,
        )
    except Exception as exc:
        return [], f"{entry['file']}: failed to parse ({exc})"

    fatal = [d.spelling for d in tu.diagnostics if d.severity >= 4]
    error = f"{entry['file']}: {fatal[0]}" if fatal else None

    def add(
        cursor: Cursor,
        check: str,
        message: str,
        fix: tuple[Edit, ...] | None = None,
    ) -> None:
        add_at(cursor.location, check, message, fix)

    def add_at(
        location: SourceLocation,
        check: str,
        message: str,
        fix: tuple[Edit, ...] | None = None,
    ) -> None:
        f = location.file
        if f is None:
            return
        path = real(f.name)
        if in_scope(path, scope_prefixes):
            diags.add(
                Diagnostic(
                    path, location.line, location.column, check, message, fix
                )
            )

    def walk(cursor: Cursor, parent: Cursor | None) -> None:
        loc_file = cursor.location.file
        if cursor.kind != CursorKind.TRANSLATION_UNIT and (
            loc_file is None or not in_scope(loc_file.name, scope_prefixes)
        ):
            return
        if "eacp-use-auto" in checks:
            check_use_auto(cursor, parent, add)
        if "eacp-no-auto-function-return" in checks:
            check_no_auto_return(cursor, parent, add)
        if "eacp-std-function-member-default" in checks:
            check_std_function_member(cursor, parent, add)
        if "eacp-no-raw-new-delete" in checks:
            check_raw_new_delete(cursor, parent, add)
        if "eacp-no-body-comments" in checks:
            check_body_comments(cursor, tu, add_at)
        for child in cursor.get_children():
            walk(child, cursor)

    walk(tu.cursor, None)
    return list(diags), error


def apply_fixes(diags: list[Diagnostic]) -> list[tuple[str, int]]:
    """Apply each diagnostic's edits, skipping overlaps, then clang-format the
    touched lines. Returns (file, fixes applied) per changed file."""
    edits_by_file: dict[str, set[Edit]] = {}
    for diag in diags:
        if diag.fix:
            edits_by_file.setdefault(diag.file, set()).update(diag.fix)

    changed: list[tuple[str, int]] = []
    for path, edits in sorted(edits_by_file.items()):
        data = Path(path).read_bytes()
        applied: list[tuple[int, int]] = []
        touched_lines: list[int] = []
        delta = 0
        for start, end, text in sorted(edits):
            if any(start < e and end > s for s, e in applied):
                continue
            if applied and start == end == applied[-1][1] == applied[-1][0]:
                continue
            new_start = start + delta
            new_end = end + delta
            data = data[:new_start] + text.encode() + data[new_end:]
            delta += len(text) - (end - start)
            applied.append((start, end))
            touched_lines.append(data[:new_start].count(b"\n") + 1)
        if applied:
            Path(path).write_bytes(data)
            clang_format_lines(path, touched_lines)
            changed.append((path, len(applied)))
    return changed


def clang_format_lines(path: str, lines: list[int]) -> None:
    line_args = [f"--lines={line}:{line}" for line in sorted(set(lines))]
    try:
        subprocess.run(
            ["clang-format", "-i", *line_args, path],
            capture_output=True,
            cwd=REPO_ROOT,
        )
    except OSError:
        pass


NOLINT_RE = re.compile(r"NOLINT(?:NEXTLINE)?(?:\(([^)]*)\))?")


def suppressed(diag: Diagnostic, line_cache: dict[str, list[str]]) -> bool:
    if diag.file not in line_cache:
        try:
            line_cache[diag.file] = Path(diag.file).read_text(
                errors="replace"
            ).splitlines()
        except OSError:
            line_cache[diag.file] = []
    lines = line_cache[diag.file]

    def matches(text: str, nextline: bool) -> bool:
        for m in NOLINT_RE.finditer(text):
            is_nextline = text[m.start() :].startswith("NOLINTNEXTLINE")
            if nextline != is_nextline:
                continue
            if m.group(1) is None or diag.check in m.group(1):
                return True
        return False

    same = lines[diag.line - 1] if 0 < diag.line <= len(lines) else ""
    prev = lines[diag.line - 2] if diag.line >= 2 else ""
    return matches(same, nextline=False) or matches(prev, nextline=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument(
        "paths",
        nargs="*",
        help="restrict to files under these paths (default: Lib and Apps)",
    )
    parser.add_argument(
        "-p",
        dest="build_dir",
        default=str(REPO_ROOT / "build"),
        help="directory containing compile_commands.json",
    )
    parser.add_argument(
        "--checks",
        default=",".join(ALL_CHECKS),
        help="comma-separated checks to run; prefix with - to disable",
    )
    parser.add_argument("--list-checks", action="store_true")
    parser.add_argument(
        "--fix",
        action="store_true",
        help="apply automatic fixes where available, then clang-format the "
        "touched lines",
    )
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    if args.list_checks:
        for name, blurb in ALL_CHECKS.items():
            print(f"{name}: {blurb}")
        return 0

    checks: set[str] = set()
    for spec in filter(None, args.checks.split(",")):
        spec = spec.strip()
        if spec.startswith("-"):
            checks.discard(spec[1:])
        elif spec == "*":
            checks |= set(ALL_CHECKS)
        elif spec in ALL_CHECKS:
            checks.add(spec)
        else:
            print(f"unknown check: {spec}", file=sys.stderr)
            return 2

    scope_paths = args.paths or [str(REPO_ROOT / "Lib"), str(REPO_ROOT / "Apps")]
    scope_prefixes = tuple(str(Path(p).resolve()) + os.sep for p in scope_paths)

    db_path = Path(args.build_dir) / "compile_commands.json"
    if not db_path.exists():
        print(f"no compile_commands.json in {args.build_dir}", file=sys.stderr)
        return 2
    database: list[dict[str, Any]] = json.loads(db_path.read_text())
    entries = [
        e
        for e in database
        if in_scope(str(Path(e["file"]).resolve()), scope_prefixes)
    ]
    if not entries:
        print("no compile commands match the given paths", file=sys.stderr)
        return 2

    all_diags: set[Diagnostic] = set()
    errors: list[str] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.j) as pool:
        futures = [pool.submit(analyze, e, scope_prefixes, checks) for e in entries]
        for future in concurrent.futures.as_completed(futures):
            diags, error = future.result()
            all_diags.update(diags)
            if error:
                errors.append(error)

    line_cache: dict[str, list[str]] = {}
    visible = sorted(
        (d for d in all_diags if not suppressed(d, line_cache)),
        key=lambda d: (d.file, d.line, d.column, d.check),
    )
    for diag in visible:
        suffix = " (has fix)" if args.fix is False and diag.fix else ""
        print(diag.render() + suffix)
    for error in errors:
        print(f"note: {error}", file=sys.stderr)
    print(
        f"\n{len(visible)} warning(s) from {len(entries)} translation unit(s).",
        file=sys.stderr,
    )

    if args.fix and visible:
        changed = apply_fixes(visible)
        applied = sum(n for _, n in changed)
        unfixable = sum(1 for d in visible if not d.fix)
        for path, n in changed:
            print(f"fixed {n} in {os.path.relpath(path, REPO_ROOT)}")
        print(
            f"\napplied {applied} fix(es) across {len(changed)} file(s); "
            f"{unfixable} warning(s) have no automatic fix.",
            file=sys.stderr,
        )
    return 1 if visible else 0


if __name__ == "__main__":
    raise SystemExit(main())
