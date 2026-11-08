# lense

C/C++ record and vtable layout printer on clang LibTooling, plus a browser front end (`web/`) running the same core as WebAssembly.

## Build and test

Nix only sees git-tracked files: `git add` new files before `nix build`.

```
nix build                      # native build + ctest; `nix build .#web` for the wasm site
nix develop
cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build
LENSE_UPDATE=1 ctest --test-dir build -R '\.(itanium|msvc|text)$'   # regenerate goldens, review the diff
```

The manual wasm build steps are in `flake.nix` (`.#web`).

## Constraints

- Pinned to LLVM 23. Clang's C++ API breaks every major release.
- Read layouts from `ASTRecordLayout` and `ItaniumVTableContext` / `MicrosoftVTableContext`. Never parse clang's `-fdump-*` text, but compare against it (`-Xclang -fdump-record-layouts -Xclang -fdump-vtable-layouts`) when in doubt.
- `src/Model.h` has no clang dependency. The wasm builds reuse `Run.cpp` and `Analyze.cpp` unchanged.
- Model offsets are bits from the start of the outermost record.
- Tests pin targets (`x86_64-linux-gnu`, `x86_64-pc-windows-msvc`) and avoid `#include`s, so goldens are host-independent.
- The fuzz oracle compiler must be clang 23. GCC and Apple clang are advisory: they genuinely differ.
- Python tests use the standard library only.
- `web/src/render.ts` mirrors `src/Text.cpp` wording.
- The front end is dark-only, in the benmandrew.com theme. Every text colour must reach 4.5:1 on each background it can sit on. The byte-map fills are validated with the dataviz colour-vision checks (see the comment at the top of `web/src/style.css`); re-run them if a fill changes.
