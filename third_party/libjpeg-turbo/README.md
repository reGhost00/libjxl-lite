# libjpeg-turbo headers (vendored)

These files are vendored from **libjpeg-turbo**, and are used *only* to compile
the in-tree JPEG codec `lib/jpegli/`.

| File | Purpose |
|------|---------|
| `jpeglib.h` | public API + `struct jpeg_decompress_struct` / `jpeg_compress_struct` |
| `jmorecfg.h` | sample type / colour space definitions |
| `jconfig.h.in` | CMake template, expanded to `jconfig.h` by `lib/jpegli.cmake` |

## Provenance

* Upstream: <https://github.com/libjpeg-turbo/libjpeg-turbo>
* Commit: `8ecba3647edb6dd940463fedf38ca33a8e2a73d1` (version 2.1.5.1)
* This is exactly the commit that upstream `libjxl` used as the
  `third_party/libjpeg-turbo` submodule when the `lib/jpegli/` subtree was
  added to this repository (see commit `b790b282`), so the vendored headers
  match the API the in-tree `jpegli` sources were written against.

Only the three header files are vendored — **no library code**. The JPEG
encoder/decoder implementation lives in `lib/jpegli/`.

## Licensing

* The IJG parts (`jpeglib.h`, `jmorecfg.h`, `jconfig.h.in`) are under the
  IJG (Independent JPEG Group) terms, which are compatible with the BSD
  licence used by this repository.
* The libjpeg-turbo modifications are under the BSD 3-Clause licence.

See <https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md>.

## Updating

Replace the three files with the corresponding ones from a newer
libjpeg-turbo release and re-check that `lib/jpegli/` still compiles; the
`JPEG_LIB_VERSION` selected in `lib/CMakeLists.txt` gates which parts of
`jpeglib.h` are visible to the `jpegli` sources.
