# lense

`lense` is a command-line tool that prints the memory layout of every C and C++ struct, class and union defined in a source file. For each record it shows member offsets and sizes, *padding holes*, *base-class subobjects* and *vtable pointers*. On Microsoft targets it also shows the `vfptr`, `vbptr` and `vtordisp` fields. With `--vtables`, each vtable is laid out in full with its *address points* and *thunks*, and every vptr line names the vtable and slot it points at, such as `-> vtable for Derived[2]`. Each record also shows its alignment, three *type traits*, *cache-line* boundaries and a field order that needs less padding.

The tool is a C++20 program built on clang's *LibTooling* against LLVM 23. It reads layouts directly from clang's `ASTRecordLayout`, `ItaniumVTableContext` and `MicrosoftVTableContext`. No code generation runs. A class's vtable therefore appears even when the file defines no *key function* for it.

## Building

Dependencies come from a *Nix flake*. `flake.nix` pins nixpkgs-unstable through `flake.lock`, which supplies LLVM and clang 23.1.3, GCC 16.2, CMake, Ninja and Python 3.

```sh
nix build            # builds and runs the full test suite; binary at result/bin/lense
nix develop          # shell with every dependency
cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build
nix run . -- file.cpp
nix build .#wasm     # WebAssembly build; runs the test suite against the Node.js build
nix build .#web      # WebAssembly build plus browser front end; static site in result/
```

Inside `nix develop` or `nix build`, CMake copies the libc and libc++ include flags from the Nix compiler wrapper in `$NIX_CC/nix-support`, so `#include <string>` resolves. Nix only sees files tracked by git. New files therefore need `git add` before `nix build`.

Without Nix, any LLVM 23 install works when passed as `-DLLVM_DIR=<prefix>/lib/cmake/llvm`. For Homebrew this is `-DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"`. CMake then finds the macOS software development kit (SDK) through `xcrun`. CMake refuses any LLVM version other than 23.

## Usage

```
lense [options] <file | -> [-- <clang arguments>]
```

Passing `-` reads the source from standard input, which is treated as C++ unless `--lang=c` is given. Everything after `--` goes to clang unchanged, for example `-- -std=c++23 -I include`.

| Option | Effect |
| --- | --- |
| `--json` | Print JavaScript Object Notation (JSON) instead of text |
| `--target=<triple>` | Use a target triple such as `x86_64-pc-windows-msvc`, which switches to the Microsoft application binary interface (ABI) |
| `--compare=<triple>` | Also lay out every record for a second target and print the two side by side. Vtables are not printed in this mode. Clang takes the last `--target`, so this overrides a target given after `--` |
| `--record=<name>` | Only show the named records, by qualified name; repeatable or comma-separated |
| `--vtables` | Also print vtable layouts, and after each vptr the vtable slot it points at. Off by default |
| `--expand-system` | Expand members whose type comes from a system header; these are collapsed by default, so `std::string` shows as one 24-byte field |
| `--source` | Print each record's definition, with line numbers, above its layout. With colour on, keywords, literals and comments are highlighted. An instantiation shows its template. In JSON, each record gains a `source` object with `line` and `text` |
| `--cache-line=<bytes>` | Mark cache-line boundaries every `<bytes>` bytes. The default is 64, and 0 turns them off. The value must be a power of two; any other value is a usage error |
| `--lang=c\|c++` | Language of standard input |
| `--color[=false]` | Colour the output; auto-detected by default |

The exit status is 0 on success, 1 when clang reported errors, and 2 for a usage error. Clang errors do not stop the output: every valid record is still printed.

### Reading the text output

Offsets, sizes and alignments are in bytes. A position written `5:6` means byte 5, bit 6. A size ending in `b` is in bits, which covers bitfields and bit-level padding.

Given this input in `t.cpp`:

```cpp
struct Base { virtual ~Base(); virtual void f(); int a; };
struct Other { virtual void g(); char c; };
struct Derived : Base, Other { void f() override; double d; char e; };
```

`lense --vtables --record=Derived t.cpp` prints:

```
struct Derived  size 48, align 8 from base Base, data size 41, padding 18 bytes
  is_standard_layout no, is_trivially_copyable no, has_unique_object_representations no
  offset  size  align
       0    12      8  Base  primary base
       0     8      8    vptr  Base  -> vtable for Derived[2]
       8     4      4    int a
      12     4         padding
      16     9      8  Other  base
      16     8      8    vptr  Other  -> vtable for Derived[7]
      24     1      1    char c
      25     7         padding
      32     8      8  double d
      40     1      1  char e
      41     7         padding
  reordering the fields saves 8 bytes (size 40): e, d

vtable for Derived  8 entries
   0  offset_to_top 0
   1  RTTI Derived
      address point: Base @0, Derived @0
   2  Derived::~Derived() [complete]
   3  Derived::~Derived() [deleting]
   4  void Derived::f()

   5  offset_to_top -16
   6  RTTI Derived
      address point: Other @16
   7  void Other::g()
```

Base subobjects are shown at their *non-virtual size*. `Base` occupies 12 bytes here against a `sizeof` of 16, so the 4 bytes at offset 12 are padding that belongs to `Derived`. The `Other` vptr at offset 16 points at slot 7, inside the second address point of the same vtable. Without `--vtables`, the output stops before the vtable, and each vptr line ends after its class name (`vptr  Base`). The header also names what set the alignment, the second line lists type traits, and the line after the members suggests a better field order. The next section explains each of these notes.

### Layout notes

**Header.** `align 8 from base Base` names what set the record's alignment. A member appears as its field name, `base X`, `virtual base X`, or `vptr` (`vfptr` on Microsoft targets). An attribute appears as written, such as `alignas(16)`, `__attribute__((aligned(32)))` or `__declspec(align(16))`. Packing appears as `__attribute__((packed))`, or as `#pragma pack(N)` for both the pragma and `-fpack-struct=N`. A class with virtual bases can be smaller as a base subobject, and its header then adds `non-virtual size` and `non-virtual align`. Records with bitfields say `little-endian` or `big-endian`. A bit position such as `0:3` counts from the least significant bit on little-endian targets and from the most significant bit on big-endian ones.

**Traits.** In C++, the second line gives clang's answers to three standard type traits. `is_standard_layout` means `offsetof` is defined for the record. `is_trivially_copyable` means `memcpy` copies its value. `has_unique_object_representations` means equal values have equal bytes, so hashing or comparing the raw bytes is safe. It is false when the record has padding or a floating-point member.

**Alignment column.** `align` is the alignment the member asks for, from its type and any `alignas` on it. Packing does not lower it. The column is blank for bitfields and padding.

**Unaligned members.** `(unaligned)` marks a member that is not guaranteed to sit at a multiple of its alignment. Either its offset or the alignment of the whole record is too small. Under `__attribute__((packed))`, `struct { int i; }` flags `i` at offset 0, because the struct itself can sit at any address.

**Memory locations.** `location N` on a bitfield numbers its C++ *memory location* ([intro.memory](https://eel.is/c++draft/intro.memory)), which is a maximal run of adjacent nonzero-width bitfields. A non-bitfield member or a zero-width bitfield ends a run. Two threads may write to different locations at the same time. Writes from two threads to two bitfields in one location are a *data race*. `lense` numbers locations by this language rule and ignores the access widths that clang's code generator picks.

**Cache lines.** A row such as `--- cache line 1 (byte 64) ---` comes before the first member at or past each boundary. A member that spans a boundary ends with `(crosses cache line)`. Boundaries count from the start of the record, as if the record started on a cache line (`pahole` does the same). `Derived` is 48 bytes, so the example has no boundary.

**Reordering.** `reordering the fields saves 8 bytes (size 40): e, d` gives a field order that needs less padding. `lense` tries every order when there are up to 8 movable units, and two heuristics otherwise. Clang then lays out a copy of the record in the best order, and `lense` reports clang's size. Adjacent bitfields move together, and a flexible array member stays last. Unions and classes with virtual bases get no suggestion. In this example, `e` moves into the tail padding of `Other`.

In JSON, each report gains `endian` and `cache_line`. Each record gains `align_source`, `traits` and `reorder`. The `reorder` object holds the new `size`, the `order` as indices into the fields in declaration order, and the field names in `fields`. Each member gains `align`, `unaligned`, `crosses_cache_line` and `memory_location`.

### Comparing targets

`--compare` lays out every record a second time for another target and prints both layouts side by side. Given this command:

```sh
printf 'struct Msg { char tag; long len; char flags; };\n' | lense --target x86_64-linux-gnu --compare x86_64-pc-windows-msvc -
```

`lense` prints:

```
struct Msg
  x86_64-unknown-linux-gnu       size 24, align 8 from len, padding 14 bytes
  x86_64-pc-windows-msvc19.33.0  size 12, align 4 from len, padding 6 bytes
            x86_64-unknown-linux-gnu            x86_64-pc-windows-msvc19.33.0
            offset  size  align            offset  size  align
                 0     1      1                 0     1      1  char tag
*                1     7                        1     3         padding
*                8     8      8                 4     4      4  long len
*               16     1      1                 8     1      1  char flags
*               17     7                        9     3         padding
```

Each target gets one column group, and `*` marks the rows that differ. Members are matched across targets by kind, role, type and name. Padding is matched by the member before it. Here `long` is 8 bytes on Linux and 4 on Windows, so `Msg` shrinks from 24 bytes to 12. A record laid out identically on both targets prints `same on both targets: size N, align N`. A record defined on only one target prints `only on <triple>`. With `--json`, the output is `{"reports": [first, second]}`.

## In the browser

The same core also runs in a browser. Clang 23's libraries are compiled to `wasm32` with *Emscripten* 6.0.9, without any LLVM back ends (`LLVM_TARGETS_TO_BUILD` is empty). `lense` is then compiled twice. `lense-web.js` is an ECMAScript module for browsers, and `lense.js` is the same command-line tool for Node.js, so the whole test suite runs against WebAssembly. All 36 golden tests and the end-to-end suite pass there, 37 tests in all. The golden output is byte-identical to the native build.

`nix build .#web` builds everything and leaves a static site in `result/`. Any static file server can host it, provided it serves `.wasm` files as `application/wasm`. For development, build the WebAssembly LLVM and `lense` with `emcmake` as listed in `CLAUDE.md`, ending with `ninja -C build-wasm/lense`. Then start the *Vite* development server:

```sh
cd web && npm install && npm run dev
```

`LENSE_WASM_DIR` points Vite at another build. Its default is `../build-wasm/lense/wasm`. `LENSE_EXAMPLES_DIR` sets where the Example menu reads its programs. Its default is `../tests/cases`.

The page has a *CodeMirror* editor on the left. On the right, each record gets a *byte map*, a member table and its vtables. The byte map shows 8 bytes per row. Bitfields are slivers inside a byte, padding is hatched, and colour marks the role: own member, inherited member, or `vptr`, `vbptr` and `vtordisp`. The member table uses the same wording as the text output. Hovering over a member or base highlights it in both the map and the table. A vptr's `-> vtable[slot]` link jumps to that slot.

The page shows the same notes as the text output. The member table has an align column, and each record shows its traits line and any reordering suggestion. *Chips* mark unaligned members and cache-line crossings, and dashed lines mark cache-line boundaries in both the byte map and the table. Union members share bytes, so the byte map draws them inside one outlined group. A union inside another record has its name on the group, such as `(anonymous union)`. In the member table, a vertical rule runs beside the union's members. Compare mode shows both byte maps and one merged table, with changed rows marked.

Clang's errors appear as editor diagnostics. An error inside a header shows on the `#include` line that pulled it in. The Example menu loads the sample program or any golden-test input, and switches the language to C for `plain.c`. The options cover language, standard, target, vtables, expansion of system types and extra clang flags. Vtables are off by default, as in the command-line tool. The "Compare with" and "Cache line" selects set `--compare` and `--cache-line`. The whole state lives in the uniform resource locator (URL) hash, so a link reproduces the view. Analysis runs in a *Web Worker*, off the page's main thread.

Sizes below are raw and after `brotli -q 11`. `lense-web.wasm` is 22.0 MB raw and 5.1 MB compressed, and 17.4 MB of it is code. The abstract syntax tree (AST) and constant evaluation take 5.4 MB, semantic analysis (Sema) 4.1 MB, clang's bytecode `constexpr` interpreter 1.4 MB and the driver 1.1 MB. `lense-web.data` holds 1,497 headers in 9.1 MB raw and 0.74 MB compressed: libc++ 6.2 MB, clang's resource headers 2.4 MB and the C library 0.5 MB. Staging leaves out clang's headers for targets the page does not offer (GPU offload, OpenCL, HLSL, PowerPC, s390, Hexagon, LoongArch, MIPS, z/OS), which saves 4.25 MB, and strips comments while keeping line numbers. Every standard header gives byte-identical output with and without that pruning. A first visit downloads about 5.9 MB with Brotli.

Timings were measured in Chrome against a local server. Clang is ready and the sample is analysed about 0.4 s after navigation. The first analysis that includes `<string>` and `<map>` takes 1.15 s, and later edits take 2–70 ms.

## Testing

`ctest --test-dir build` runs 39 tests across four layers. The Python scripts use only the standard library.

**Golden files.** Inputs live in `tests/cases` and expected output in `tests/expected`. The 12 cases give 36 tests, because each case runs as JSON on `x86_64-linux-gnu`, as JSON on `x86_64-pc-windows-msvc`, and as text on Linux. `layout_notes.cpp` covers alignment sources, unaligned members, memory locations, cache lines and reordering. To regenerate the expected output, run `LENSE_UPDATE=1 ctest --test-dir build` and review the git diff.

**End-to-end tests.** `tests/e2e/test_cli.py` holds 38 tests that run the real binary. They cover file and stdin input, C and C++ modes, `--record`, `--vtables`, `--source`, `--target` and `--` pass-through. They also cover the layout notes, `--cache-line` and `--compare`. They also check quote includes relative to the file, which function bodies are parsed, system-type collapsing, colour, exit codes, compile errors and binary input. The rest check determinism and that the text output agrees with the JSON.

**Oracle fuzzing.** `tests/fuzz/fuzz.py --mode oracle` runs *property-based* fuzzing against a compiled *oracle*. `gen.py` generates random valid class hierarchies with virtual and repeated bases, virtual functions, bitfields, unions, arrays, `[[no_unique_address]]`, `alignas` and packing. `lense` reports on each program, and `oracle.py` turns the report into a C++ checker that is compiled and run. The checker tests `sizeof`, `alignof` and the address of every base and field. It checks every bitfield's bits by assigning -1 and diffing memory, and it checks every vptr value. Every vtable entry is checked relative to its address point, covering offsets, run-time type information (RTTI) pointers and function slots. Each function slot is called through any thunk, and the checker verifies the returned *final-overrider* id and its complete-object `this`. It also checks the three traits against `<type_traits>` and the alignment of every direct named field. For each reordering suggestion, the fuzzer builds a copy of the program with that class's fields in the suggested order and checks its `sizeof` against the reported size.

`props.py` checks structural invariants. Holes match padding, every bit is either padding or a member, and children nest inside their parents. Each Itanium sub-table has one address point, preceded by RTTI and by an `offset_to_top` equal to minus the subobject offset. The fields that the layout notes add have invariants of their own. Cross targets (x86-64 Linux and Microsoft) get the invariants plus `static_assert` checks, which test the traits with compiler builtins. Failures are *shrunk* to a minimal program and saved with the report and checker. Every case reproduces from its seed:

```sh
tests/fuzz/fuzz.py --lense build/lense --cxx clang++ --seed N --iterations 1
```

**Robustness fuzzing.** `--mode mutate` corrupts generated programs by deleting, duplicating, swapping or truncating lines and by inserting tokens. Each run must finish without a crash and emit valid JSON. The invariants must also hold on whatever records survive.

CTest runs 64 oracle cases and 200 mutation cases with seed 1. A campaign of 1,500 oracle cases ran in 463 s with 0 failures, and clang rejected 45 of the generated programs, which were discarded. A campaign of 2,000 mutation cases ran in 46 s with 0 failures.

A later campaign of 3,000 oracle cases with the trait, alignment and reordering checks found no `lense` bug. Two cases failed to link, because macOS `ld` rejects a packed struct with a vptr inside an `inline static` object. This is a generator problem, and the fuzz code without the new checks hits it too. A campaign of 1,000 mutation cases had 0 failures.

```
tests/fuzz/fuzz.py --lense build/lense --cxx clang++ --seed 100000 --iterations 1500
tests/fuzz/fuzz.py --lense build/lense --mode mutate --seed 200000 --iterations 2000
```

During development the fuzzer found three bugs, all now fixed. Address points of vtables with no functions were attributed to the wrong sub-table, and the text view printed them in the wrong table. `[[no_unique_address]]` members were shown at their `sizeof` instead of their *data size*.

## Limits

- Arrays of records show as one field, and element padding is not broken out.
- Microsoft *vbtable* contents are not shown.
- Only records defined in the main file are listed.
- Uninstantiated templates have no layout. Instantiate the specialisation you want, for example `template struct Box<int>;`.
- Records with compile errors are skipped.
- Field reordering is not suggested for unions or for records with virtual bases.
- Bitfield memory locations follow the language rule, so they can differ from the widths of the loads and stores clang emits.
- In the browser, the standard headers are Emscripten's libc++ on musl for every target. `std::` types therefore have libc++ layouts, so `std::string` is 24 bytes where GCC's libstdc++ makes it 32. `#include <string>` fails on Microsoft and Apple targets. Code without includes works on every target.
- In the browser, `arm_neon.h`, `arm_sve.h` and `riscv_vector.h` are missing: clang generates them only when the ARM and RISC-V back ends are built, and the WebAssembly build has no back ends.
- The oracle compiler must be clang 23, the layout engine `lense` reports on. GCC and Apple clang are *advisory*, so their disagreements are printed and saved without failing the run.

They do disagree. `lense` describes upstream clang 23's layout, which can differ from the compiler a project ships with. A packed class with non-plain-old-data (non-POD) members, such as `struct __attribute__((packed)) T3 : T1 {...}` with a polymorphic `T1`, has size 96 and alignment 8 under clang 23. Apple clang 21 gives size 92 and alignment 1, and GCC 16 does not pack non-POD members either. A struct holding only `int : 1;` has size 1 and alignment 1 under clang, against 4 and 4 under GCC. To match an older clang, try passing `-- -fclang-abi-compat=<version>`.

## Source layout

- `src/Model.h` holds the data model, which has no clang dependency.
- `src/Analyze.cpp` converts the abstract syntax tree (AST) into the model.
- `src/Json.cpp` and `src/Text.cpp` are the two printers.
- `src/Run.cpp` runs clang on a string.
- `src/main.cpp` is the command-line interface.
- `src/Wasm.cpp` is the browser entry point. `lense_analyze(code, options)` returns JSON.
- `wasm/` holds the Emscripten CMake files: the LLVM cache file `llvm-cache.cmake` and the header staging.
- `web/` is the browser front end.

Every layout `lense` prints is the one clang 23 computed for the chosen target. The tool only decides how much of it to show.
