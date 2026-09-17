# miniz

[miniz](https://github.com/richgel999/miniz) 3.1.2, the amalgamated
`miniz.h`/`miniz.c` pair from the upstream release archive, unmodified. MIT
licensed; `LICENSE` is upstream's.

Only `eacp-core` links it, and PRIVATE, so no eacp header and no consumer ever
sees a miniz symbol: the whole of it is reached through `eacp::Zip`
(`Lib/eacp/Core/Utils/Zip.h`). It is compiled with `MINIZ_NO_STDIO`, so it has
no file I/O of its own — archives are read through `MemoryMappedFile` and
written through `Files::writeFile`, which keeps UTF-8 paths on one code path.

To update: replace the three files with the next release's, and change the
version above.
