# fmt

[fmt](https://github.com/fmtlib/fmt) 12.2.0: the `include/fmt` headers and
`src/format.cc` from the upstream release archive, unmodified, and nothing else
of it (no `os.cc`, no module unit, no C API source, none of its CMake). MIT
licensed, with an optional exception that lets compiled binaries omit the
notice (stated at the top of `include/fmt/format.h`); `LICENSE` is upstream's.

Only `eacp-core` links it, and PRIVATE, so no eacp header and no consumer ever
sees an fmt symbol. It is built as its own static library rather than
header-only, so the formatting core is compiled once. Exactly one eacp source
includes it, `Lib/eacp/Core/Utils/Format.cpp`, behind `eacp::Strings::format`.
On MSVC fmt's `base.h` requires `/utf-8` of every translation unit that
includes it: the fmt target sets it PRIVATE on its own `format.cc`, and
`eacp-core` sets it on `Format.cpp` alone, so it reaches nothing else.

To update: replace `include/fmt` and `src/format.cc` with the next release's,
and change the version above.
