# lense

`lense` is a command-line tool that prints the memory layout of every C and C++ struct, class and union defined in a source file. For each record it shows member offsets and sizes, *padding holes*, *base-class subobjects* and *vtable pointers*. On Microsoft targets it also shows the `vfptr`, `vbptr` and `vtordisp` fields. Each vtable is laid out in full with its *address points* and *thunks*, and every vptr line names the vtable and slot it points at, such as `-> vtable for Derived[2]`.

The tool is a C++20 program built on clang's *LibTooling* against LLVM 23. It reads layouts directly from clang's `ASTRecordLayout`, `ItaniumVTableContext` and `MicrosoftVTableContext`. No code generation runs. A class's vtable therefore appears even when the file defines no *key function* for it.

## Building

Dependencies come from a *Nix flake*. `flake.nix` pins nixpkgs-unstable through `flake.lock`, which supplies LLVM and clang 23.1.3, GCC 16.2, CMake, Ninja and Python 3.

```sh
nix build            # builds and runs the full test suite; binary at result/bin/lense
nix develop          # shell with every dependency
cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build
nix run . -- file.cpp
nix build .#wasm     # WebAssembly build; runs the test suite against the Node.js build
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
| `--record=<name>` | Only show the named records, by qualified name; repeatable or comma-separated |
| `--no-vtables` | Leave out vtable layouts |
| `--expand-system` | Expand members whose type comes from a system header; these are collapsed by default, so `std::string` shows as one 24-byte field |
| `--lang=c\|c++` | Language of standard input |
| `--color[=false]` | Colour the output; auto-detected by default |

The exit status is 0 on success, 1 when clang reported errors, and 2 for a usage error. Clang errors do not stop the output: every valid record is still printed.

### Reading the text output

Offsets and sizes are in bytes. A position written `5:6` means byte 5, bit 6. A size ending in `b` is in bits, which covers bitfields and bit-level padding.

Given this input in `t.cpp`:

```cpp
struct Base { virtual ~Base(); virtual void f(); int a; };
struct Other { virtual void g(); char c; };
struct Derived : Base, Other { void f() override; double d; char e; };
```

`lense --record=Derived t.cpp` prints:

```
struct Derived  size 48, align 8, data size 41, padding 18 bytes
  offset  size
       0    12  Base  primary base
       0     8    vptr  Base  -> vtable for Derived[2]
       8     4    int a
      12     4  padding
      16     9  Other  base
      16     8    vptr  Other  -> vtable for Derived[7]
      24     1    char c
      25     7  padding
      32     8  double d
      40     1  char e
      41     7  padding

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

Base subobjects are shown at their *non-virtual size*. `Base` occupies 12 bytes here against a `sizeof` of 16, so the 4 bytes at offset 12 are padding that belongs to `Derived`. The `Other` vptr at offset 16 points at slot 7, inside the second address point of the same vtable.

## Testing

`ctest --test-dir build` runs 33 tests across four layers. The Python scripts use only the standard library.

**Golden files.** Inputs live in `tests/cases` and expected output in `tests/expected`, giving 30 tests. Each case runs as JSON on `x86_64-linux-gnu`, as JSON on `x86_64-pc-windows-msvc`, and as text on Linux. To regenerate the expected output, run `LENSE_UPDATE=1 ctest --test-dir build` and review the git diff.

**End-to-end tests.** `tests/e2e/test_cli.py` holds 20 tests that run the real binary. They cover file and stdin input, C and C++ modes, `--record`, `--no-vtables`, `--target` and `--` pass-through. They also check quote includes relative to the file, system-type collapsing, colour, exit codes, compile errors and binary input. The rest check determinism and that the text output agrees with the JSON.

**Oracle fuzzing.** `tests/fuzz/fuzz.py --mode oracle` runs *property-based* fuzzing against a compiled *oracle*. `gen.py` generates random valid class hierarchies with virtual and repeated bases, virtual functions, bitfields, unions, arrays, `[[no_unique_address]]`, `alignas` and packing. `lense` reports on each program, and `oracle.py` turns the report into a C++ checker that is compiled and run. The checker tests `sizeof`, `alignof` and the address of every base and field. It checks every bitfield's bits by assigning -1 and diffing memory, and it checks every vptr value. Every vtable entry is checked relative to its address point, covering offsets, run-time type information (RTTI) pointers and function slots. Each function slot is called through any thunk, and the checker verifies the returned *final-overrider* id and its complete-object `this`.

`props.py` checks structural invariants. Holes match padding, every bit is either padding or a member, and children nest inside their parents. Each Itanium sub-table has one address point, preceded by RTTI and by an `offset_to_top` equal to minus the subobject offset. Cross targets (x86-64 Linux and Microsoft) get the invariants plus `static_assert` checks. Failures are *shrunk* to a minimal program and saved with the report and checker. Every case reproduces from its seed:

```sh
tests/fuzz/fuzz.py --lense build/lense --cxx clang++ --seed N --iterations 1
```

**Robustness fuzzing.** `--mode mutate` corrupts generated programs by deleting, duplicating, swapping or truncating lines and by inserting tokens. Each run must finish without a crash and emit valid JSON. The invariants must also hold on whatever records survive.

CTest runs 64 oracle cases and 200 mutation cases with seed 1. A campaign of 1,500 oracle cases ran in 463 s with 0 failures, and clang rejected 45 of the generated programs, which were discarded. A campaign of 2,000 mutation cases ran in 46 s with 0 failures.

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

A browser front end that runs the same core compiled to WebAssembly is planned.

Every layout `lense` prints is the one clang 23 computed for the chosen target. The tool only decides how much of it to show.
