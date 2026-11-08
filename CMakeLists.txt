cmake_minimum_required(VERSION 3.20)
project(lense LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Clang's C++ API changes between major releases, so pin the version.
set(LENSE_LLVM_MAJOR 23)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL LENSE_LLVM_MAJOR)
  message(FATAL_ERROR "lense needs LLVM ${LENSE_LLVM_MAJOR}, found "
    "${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}. Set LLVM_DIR to an LLVM "
    "${LENSE_LLVM_MAJOR} install, or use `nix develop`.")
endif()
# NO_CMAKE_FIND_ROOT_PATH: cross toolchains (Emscripten) would look for the
# hint inside their sysroot.
find_package(Clang REQUIRED CONFIG HINTS "${LLVM_LIBRARY_DIR}/cmake/clang"
             NO_CMAKE_FIND_ROOT_PATH)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# lense runs clang in-process, so it must be told where the headers are that
# the clang driver (or a Nix compiler wrapper) would normally supply. All of
# this is baked into a generated header, lense_config.h.

# Clang's builtin headers (stddef.h, ...) sit next to libclang-cpp, in
# <libdir>/clang/<major>. Under Nix that is clang's own lib output, not LLVM's.
set(_res "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
if(TARGET clang-cpp)
  get_target_property(_clang_cpp clang-cpp LOCATION)
  get_filename_component(_clang_libdir "${_clang_cpp}" DIRECTORY)
  set(_res "${_clang_libdir}/clang/${LLVM_VERSION_MAJOR}")
endif()
set(LENSE_RESOURCE_DIR "${_res}" CACHE PATH "Clang resource directory")
if(EMSCRIPTEN)
  # wasm/CMakeLists.txt copies these headers into the WebAssembly builds.
elseif(NOT EXISTS "${LENSE_RESOURCE_DIR}/include/stddef.h")
  message(WARNING "No clang builtin headers in ${LENSE_RESOURCE_DIR}; "
                  "set LENSE_RESOURCE_DIR")
endif()

# On macOS the C and C++ standard library headers live in the SDK.
set(LENSE_DEFAULT_SYSROOT "" CACHE PATH "Default -isysroot for Apple targets")
if(APPLE AND NOT LENSE_DEFAULT_SYSROOT)
  if(DEFINED ENV{SDKROOT} AND EXISTS "$ENV{SDKROOT}")
    set(_sdk "$ENV{SDKROOT}")
  else()
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE _sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
  endif()
  set(LENSE_DEFAULT_SYSROOT "${_sdk}" CACHE PATH "" FORCE)
endif()

# Inside a Nix build or `nix develop`, the compiler wrapper ($NIX_CC) adds the
# libc and libc++ include paths. Copy those flags so lense sees the same
# headers as the compiler.
set(LENSE_HOST_ARGS "" CACHE STRING
    "Clang arguments for host header search, added when no --target is given")
if(NOT LENSE_HOST_ARGS AND DEFINED ENV{NIX_CC})
  set(_args "")
  foreach(_f libc-cflags libcxx-cxxflags)
    set(_path "$ENV{NIX_CC}/nix-support/${_f}")
    if(EXISTS "${_path}")
      file(READ "${_path}" _content)
      separate_arguments(_flags UNIX_COMMAND "${_content}")
      list(FILTER _flags EXCLUDE REGEX "^-fmacro-prefix-map=")
      list(APPEND _args ${_flags})
    endif()
  endforeach()
  set(LENSE_HOST_ARGS "${_args}" CACHE STRING "" FORCE)
endif()

set(LENSE_HOST_ARGS_INIT "")
foreach(_a IN LISTS LENSE_HOST_ARGS)
  string(REPLACE "\\" "\\\\" _a "${_a}")
  string(REPLACE "\"" "\\\"" _a "${_a}")
  string(APPEND LENSE_HOST_ARGS_INIT "\"${_a}\", ")
endforeach()
configure_file(src/lense_config.h.in lense_config.h @ONLY)
message(STATUS "lense resource dir: ${LENSE_RESOURCE_DIR}")
message(STATUS "lense sysroot: ${LENSE_DEFAULT_SYSROOT}")
message(STATUS "lense host args: ${LENSE_HOST_ARGS}")

# Run.cpp reads lense_config.h. The WebAssembly builds compile it once per
# program, because each finds its headers in a different place.
add_library(lense_core STATIC
  src/Analyze.cpp
  src/Json.cpp
  src/Text.cpp
)
if(NOT EMSCRIPTEN)
  target_sources(lense_core PRIVATE src/Run.cpp)
endif()
target_include_directories(lense_core PUBLIC src
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(lense_core SYSTEM PUBLIC
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(lense_core PUBLIC ${LLVM_DEFINITIONS})
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(lense_core PUBLIC -fno-rtti)
endif()
target_compile_options(lense_core PRIVATE -Wall -Wextra)

if(TARGET clang-cpp AND LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(lense_core PUBLIC clang-cpp LLVM)
else()
  target_link_libraries(lense_core PUBLIC
    clangTooling clangFrontend clangSerialization clangSema clangAST
    clangBasic LLVMSupport)
endif()

add_executable(lense src/main.cpp)
target_link_libraries(lense PRIVATE lense_core)
install(TARGETS lense RUNTIME DESTINATION bin)

# The program the tests run.
if(EMSCRIPTEN)
  add_subdirectory(wasm)
  # POST_BUILD steps must be added in the directory that created the target.
  add_custom_command(TARGET lense POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DJS=$<TARGET_FILE:lense>"
            -P "${PROJECT_SOURCE_DIR}/wasm/patch-noderawfs.cmake")
  set(LENSE_TEST_EXE "${PROJECT_BINARY_DIR}/lense-node")
else()
  set(LENSE_TEST_EXE "$<TARGET_FILE:lense>")
endif()

enable_testing()
add_subdirectory(tests)
