# lense

CLI that prints C/C++ record layouts and vtable layouts, built on clang LibTooling, plus a browser front end running the same core as WebAssembly.

## Build and test

Dependencies come from `flake.nix` (nixpkgs-unstable pinned in `flake.lock`). Nix only sees git-tracked files: `git add` new files before `nix build`.

```
nix build                                   # build + full ctest in the sandbox
nix develop                                 # then:
cmake -S . -B build -G Ninja && ninja -C build
ctest --test-dir build                      # goldens, e2e, fuzz (39 tests)
LENSE_UPDATE=1 ctest --test-dir build -R '\.(itanium|msvc|text)$'   # regenerate goldens, review the diff
tests/fuzz/fuzz.py --lense build/lense --cxx "$LENSE_ORACLE_CXX" --advisory-cxx "$LENSE_ADVISORY_CXX" --static-cxx "$LENSE_STATIC_CXX" --iterations 1000
```

WebAssembly (outside Nix; `nix build .#web` does all of this):

```
emcmake cmake -G Ninja -S <llvm-project>/llvm -B build-wasm/llvm -C wasm/llvm-cache.cmake -DLLVM_NATIVE_TOOL_DIR=<llvm-tblgen dir>
ninja -C build-wasm/llvm clangTooling clang-resource-headers
emcmake cmake -G Ninja -S . -B build-wasm/lense -DCMAKE_BUILD_TYPE=MinSizeRel -DLLVM_DIR=$PWD/build-wasm/llvm/lib/cmake/llvm
ninja -C build-wasm/lense && ctest --test-dir build-wasm/lense   # tests run build-wasm/lense/lense-node
cd web && npm install && npm run dev        # LENSE_WASM_DIR defaults to ../build-wasm/lense/wasm
```

## Tests

- `tests/cases` + `tests/expected`: golden files, run as Itanium JSON, MSVC JSON, text.
- `tests/e2e/test_cli.py`: unittest suite driving the real binary (`$LENSE`).
- `tests/fuzz/`: `gen.py` random valid programs (+ shrinking), `oracle.py` JSON report -> runtime checker C++ (offsets, bitfield bits, vptr values, vtable entries called through thunks), `props.py` structural invariants, `fuzz.py` driver (`--mode oracle|mutate`). Failures land in `fuzz-failures/` with a reproducing seed.
- Oracle mode builds one runtime checker per `--batch` cases (default 16, each case in its own namespace). macOS checks each new executable before its first run, one at a time (about 150 ms each), so one binary per case caps throughput at about 3 cases/s whatever `--jobs` is. A case that fails in a batch is rerun alone before it is reported.
- The oracle compiler must be clang 23. GCC and Apple clang are advisory only: they genuinely differ (packed non-POD members, unnamed-bitfield-only structs).
- Python tests use the standard library only.

## Decisions and constraints

- C++20, pinned to LLVM 23. Clang's C++ API changes every major release; bumping the version means fixing API breaks (e.g. `PrintingPolicy::AnonymousTagNameStyle`, `FieldDecl::getBitWidthValue()`).
- Read layouts from `ASTRecordLayout` and `ItaniumVTableContext` / `MicrosoftVTableContext`. Never parse clang's `-fdump-*` text. libclang (C API) has no vtable API, so Python/Rust bindings are not an option.
- `src/Model.h` has no clang dependency. Analyze fills it; Json and Text print it. The WebAssembly builds reuse `Run.cpp` + `Analyze.cpp` unchanged.
- Traversal order and naming follow clang's own dumps (`RecordLayoutBuilder.cpp` `DumpRecordLayout`, MS vftable names from `PathToIntroducingObject` reversed). When in doubt, compare against `clang++ -S -o /dev/null -Xclang -fdump-record-layouts -Xclang -fdump-vtable-layouts`.
- All offsets in the model are bits from the start of the outermost record. Padding is computed by sweeping occupied leaf ranges, then each hole is placed in the innermost non-empty record that contains it.
- Base subobjects are sized by non-virtual size, `[[no_unique_address]]` fields by data size (tail padding may hold later members); empty ones are size 0.
- Header search is baked in at configure time into `lense_config.h`: resource dir (next to libclang-cpp), macOS SDK (`$SDKROOT` or `xcrun`), and `LENSE_HOST_ARGS` copied from `$NIX_CC/nix-support/{libc-cflags,libcxx-cxxflags}`. Host flags are skipped for non-host `--target`s.
- Classes local to a function are named from it (`f(int)::S`, `h<1>()::S::In`, `(lambda at L:C)::operator()()::S`), because clang prints them unqualified and names would clash.
- Function bodies outside the main file are skipped (`SkipFunctionBodies` + `Consumer::shouldSkipFunctionBody`): only main-file records are reported, and clang still parses constexpr and deduced-return bodies. Errors in skipped bodies go unreported. Main-file bodies are always parsed, for local classes.
- LLVM already registers `--color`; reuse it via `llvm::WithColor` instead of defining another.
- Vtable address points belong to the sub-table holding the RTTI entry before them; a table with no functions has its address point at the next table's start.
- A node's `align` is what the member asks for (type alignment raised by `alignas` on it), not what packing leaves it. `unaligned` means gcd(offset, outermost record's align) < align, so a packed record's member at offset 0 is unaligned too.
- Bitfield `memory_location` follows the language rule ([intro.memory]: a maximal run of adjacent nonzero-width bitfields), not clang CodeGen's access units, so lense does not link clangCodeGen (wasm size).
- Field reordering estimates orders itself (every order up to 8 units, else heuristics), then lays the best out with clang on a synthetic copy of the record (same bases, attributes, a dummy virtual for an own vptr) and reports clang's size. No suggestion for unions or records with virtual bases; adjacent bitfields move as one unit; a flexible array stays last.
- `--compare` runs clang twice; the second run appends `--target=`, which wins because clang takes the last one. Members are matched across targets by kind, role, type and name; padding by the member it follows.
- Tests pin targets (`x86_64-linux-gnu`, `x86_64-pc-windows-msvc`) and avoid `#include`s so goldens are host-independent.
- WebAssembly: `src/Wasm.cpp` is the browser entry (`lense_analyze`); `src/main.cpp` + `Run.cpp` also build as `lense.js` for Node so every test suite runs against wasm. Headers come from Emscripten's sysroot, staged by `wasm/stage-headers.cmake`, which patches libc++ `__config_site` from ABI v2 to v1 to match native targets.
- Emscripten resolves the `.data` package against the page or worker location. The worker passes `locateFile` so it resolves next to `lense-web.js`; without it Vite's HTML fallback is loaded as the package and every header is empty.
- Front end: TypeScript + Vite + CodeMirror 6; `web/src/render.ts` mirrors `src/Text.cpp` wording. Chart colours come from the dataviz reference palette, validated for colour-vision deficiency in both modes.
