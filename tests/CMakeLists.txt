# Golden-file tests. Each case runs three ways: JSON for an Itanium target,
# JSON for a Microsoft target, and text for the Itanium target.
#
# Regenerate the expected files with:  LENSE_UPDATE=1 ctest --test-dir build
# Flags are |-separated, because a ; would split the -D argument.
set(itanium_flags "--json|--target=x86_64-linux-gnu")
set(msvc_flags "--json|--target=x86_64-pc-windows-msvc")
set(text_flags "--color=false|--target=x86_64-linux-gnu")
set(itanium_ext itanium.json)
set(msvc_ext msvc.json)
set(text_ext txt)

file(GLOB cases CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/cases/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/cases/*.c")

foreach(input ${cases})
  get_filename_component(name "${input}" NAME_WE)
  set(rc 0)
  if(name STREQUAL "broken")
    set(rc 1)
  endif()
  foreach(variant itanium msvc text)
    set(ext ${${variant}_ext})
    set(flags ${${variant}_flags})
    add_test(NAME ${name}.${variant}
      COMMAND ${CMAKE_COMMAND}
        -DLENSE=${LENSE_TEST_EXE}
        -DINPUT=${input}
        -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.${ext}
        -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/actual/${name}.${ext}
        -DFLAGS=${flags}
        -DEXPECT_RC=${rc}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_case.cmake)
  endforeach()
endforeach()

# Cached runs must report what uncached runs do. Run.cpp is only in
# lense_core for native builds.
if(NOT EMSCRIPTEN)
  add_executable(lense_preamble_test preamble_test.cpp)
  target_link_libraries(lense_preamble_test PRIVATE lense_core)
  add_test(NAME preamble COMMAND lense_preamble_test ${cases})
endif()

# End-to-end and fuzz tests need Python 3 (standard library only).
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  message(WARNING "Python 3 not found: skipping e2e and fuzz tests")
  return()
endif()

add_test(NAME e2e.cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_cli.py)
set_tests_properties(e2e.cli PROPERTIES ENVIRONMENT "LENSE=${LENSE_TEST_EXE}")

# The oracle compiler must be clang ${LENSE_LLVM_MAJOR}, the same layout
# engine lense reports on. Other compilers (GCC, Apple clang) are advisory:
# their disagreements are printed and saved but do not fail the test.
set(_oracle "$ENV{LENSE_ORACLE_CXX}")
if(NOT _oracle AND NOT EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL ${LENSE_LLVM_MAJOR} AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS ${LENSE_LLVM_MAJOR}.99)
  set(_oracle "${CMAKE_CXX_COMPILER}")
endif()
set(LENSE_ORACLE_CXX "${_oracle}" CACHE FILEPATH
    "clang++ ${LENSE_LLVM_MAJOR} used to check lense against real object code")
set(LENSE_ADVISORY_CXX "$ENV{LENSE_ADVISORY_CXX}" CACHE STRING
    "Further compilers to compare against, without failing (;-separated)")
set(LENSE_STATIC_CXX "$ENV{LENSE_STATIC_CXX}" CACHE FILEPATH
    "clang++ for static checks on cross targets (default: the oracle)")
set(LENSE_FUZZ_ITERATIONS 64 CACHE STRING "Cases per fuzz test in ctest")

set(_fuzz ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz.py
    --lense ${LENSE_TEST_EXE} --seed 1
    --out ${CMAKE_CURRENT_BINARY_DIR}/fuzz-failures)

add_test(NAME fuzz.mutate
  COMMAND ${_fuzz} --mode mutate --iterations 200)

if(LENSE_ORACLE_CXX)
  set(_oracle_args --cxx ${LENSE_ORACLE_CXX})
  foreach(_c IN LISTS LENSE_ADVISORY_CXX)
    list(APPEND _oracle_args --advisory-cxx ${_c})
  endforeach()
  if(LENSE_STATIC_CXX)
    list(APPEND _oracle_args --static-cxx ${LENSE_STATIC_CXX})
  endif()
  if(EMSCRIPTEN)
    # lense.js defaults to x86-64 Linux; the checkers run on the build host.
    execute_process(COMMAND ${LENSE_ORACLE_CXX} -print-target-triple
                    OUTPUT_VARIABLE _host OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND _oracle_args --host-target ${_host})
  endif()
  # Advisory divergences are expected (see README), so don't spend time
  # shrinking them here.
  add_test(NAME fuzz.oracle
    COMMAND ${_fuzz} --mode oracle --iterations ${LENSE_FUZZ_ITERATIONS}
            --advisory-shrink-budget 0 ${_oracle_args})
else()
  message(WARNING "No clang++ ${LENSE_LLVM_MAJOR} for the fuzz oracle; set "
                  "LENSE_ORACLE_CXX (nix develop does this)")
endif()
