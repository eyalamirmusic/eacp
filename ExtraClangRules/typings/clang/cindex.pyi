"""Minimal stubs for the slice of clang.cindex that main.py uses.

The libclang wheel builds CursorKind/TokenKind members dynamically at import
time, so pyright cannot see them without these declarations.
"""

from collections.abc import Iterator
from typing import Any, ClassVar

class Config:
    @staticmethod
    def set_library_file(filename: str) -> None: ...

class CursorKind:
    TRANSLATION_UNIT: ClassVar[CursorKind]
    UNEXPOSED_EXPR: ClassVar[CursorKind]
    VAR_DECL: ClassVar[CursorKind]
    FIELD_DECL: ClassVar[CursorKind]
    FUNCTION_DECL: ClassVar[CursorKind]
    CXX_METHOD: ClassVar[CursorKind]
    CONSTRUCTOR: ClassVar[CursorKind]
    DESTRUCTOR: ClassVar[CursorKind]
    FUNCTION_TEMPLATE: ClassVar[CursorKind]
    LAMBDA_EXPR: ClassVar[CursorKind]
    COMPOUND_STMT: ClassVar[CursorKind]
    DECL_STMT: ClassVar[CursorKind]
    CXX_CATCH_STMT: ClassVar[CursorKind]
    CXX_FOR_RANGE_STMT: ClassVar[CursorKind]
    INIT_LIST_EXPR: ClassVar[CursorKind]
    CXX_NEW_EXPR: ClassVar[CursorKind]
    CXX_DELETE_EXPR: ClassVar[CursorKind]
    def is_expression(self) -> bool: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...

class TokenKind:
    COMMENT: ClassVar[TokenKind]
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...

class File:
    @property
    def name(self) -> str: ...

class SourceLocation:
    @property
    def file(self) -> File | None: ...
    @property
    def line(self) -> int: ...
    @property
    def column(self) -> int: ...
    @property
    def offset(self) -> int: ...

class SourceRange:
    @property
    def start(self) -> SourceLocation: ...
    @property
    def end(self) -> SourceLocation: ...

class Type:
    @property
    def spelling(self) -> str: ...
    def get_canonical(self) -> Type: ...

class Token:
    @property
    def spelling(self) -> str: ...
    @property
    def kind(self) -> TokenKind: ...
    @property
    def location(self) -> SourceLocation: ...
    @property
    def extent(self) -> SourceRange: ...

class Cursor:
    @property
    def kind(self) -> CursorKind: ...
    @property
    def spelling(self) -> str: ...
    @property
    def location(self) -> SourceLocation: ...
    @property
    def extent(self) -> SourceRange: ...
    @property
    def type(self) -> Type: ...
    @property
    def result_type(self) -> Type: ...
    def get_children(self) -> Iterator[Cursor]: ...
    def get_tokens(self) -> Iterator[Token]: ...

class Diagnostic:
    @property
    def severity(self) -> int: ...
    @property
    def spelling(self) -> str: ...

class TranslationUnit:
    PARSE_INCOMPLETE: ClassVar[int]
    @property
    def cursor(self) -> Cursor: ...
    @property
    def diagnostics(self) -> Iterator[Diagnostic]: ...
    def get_tokens(self, extent: SourceRange | None = ...) -> Iterator[Token]: ...

class Index:
    @staticmethod
    def create(excludeDecls: bool = ...) -> Index: ...
    def parse(
        self,
        path: str | None,
        args: list[str] | None = ...,
        unsaved_files: Any = ...,
        options: int = ...,
    ) -> TranslationUnit: ...
