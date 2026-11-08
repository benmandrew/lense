# CMake cache for building the clang libraries that lense needs as
# WebAssembly with Emscripten:
#
#   emcmake cmake -G Ninja -S <llvm-project>/llvm -B <dir> \
#     -C wasm/llvm-cache.cmake -DLLVM_NATIVE_TOOL_DIR=<dir with llvm-tblgen>
#   ninja -C <dir> clangTooling clang-resource-headers
#
# lense only reads the AST and record layouts, so there are no LLVM
# backends, no tools and no optional libraries. The table generators must
# be native and exactly the same LLVM version.

set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "")
set(LLVM_ENABLE_PROJECTS clang CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD "" CACHE STRING "")
set(LLVM_HOST_TRIPLE wasm32-unknown-emscripten CACHE STRING "")
set(LLVM_DEFAULT_TARGET_TRIPLE x86_64-unknown-linux-gnu CACHE STRING "")

foreach(var
    LLVM_ENABLE_THREADS LLVM_ENABLE_PIC LLVM_ENABLE_ZLIB LLVM_ENABLE_ZSTD
    LLVM_ENABLE_LIBXML2 LLVM_ENABLE_LIBEDIT LLVM_ENABLE_LIBPFM
    LLVM_ENABLE_BACKTRACES LLVM_ENABLE_CRASH_OVERRIDES
    LLVM_ENABLE_UNWIND_TABLES LLVM_ENABLE_TERMINFO
    LLVM_INCLUDE_TESTS LLVM_INCLUDE_EXAMPLES LLVM_INCLUDE_BENCHMARKS
    LLVM_INCLUDE_DOCS LLVM_INCLUDE_UTILS LLVM_BUILD_TOOLS LLVM_BUILD_UTILS
    CLANG_BUILD_TOOLS CLANG_INCLUDE_TESTS CLANG_INCLUDE_DOCS
    CLANG_ENABLE_STATIC_ANALYZER CLANG_ENABLE_ARCMT
    CLANG_ENABLE_OBJC_REWRITER CLANG_PLUGIN_SUPPORT CLANG_BUILD_EXAMPLES)
  set(${var} OFF CACHE BOOL "")
endforeach()
