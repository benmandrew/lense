# WebAssembly builds, configured with emcmake against a WebAssembly build of
# LLVM (see llvm-cache.cmake):
#
#   lense.js      the command line for Node, with the host's file system. The
#                 tests run it through the generated `lense-node` script.
#   lense-web.js  an ES module for browsers exporting lense_analyze (see
#                 src/Wasm.cpp). Its headers are preloaded from lense-web.data.

set(LENSE_WASM_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/headers")
set(stamp "${CMAKE_CURRENT_BINARY_DIR}/headers.stamp")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${stamp}"
  COMMAND "${CMAKE_COMMAND}" "-DRESOURCE=${LENSE_RESOURCE_DIR}"
          "-DSYSROOT=${EMSCRIPTEN_SYSROOT}" "-DOUT=${LENSE_WASM_HEADERS}"
          "-DPYTHON=${Python3_EXECUTABLE}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/stage-headers.cmake"
  COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stage-headers.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/strip-comments.py"
  COMMENT "Staging headers for the WebAssembly builds")
add_custom_target(lense_headers DEPENDS "${stamp}")

set(common -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB)

# Under Node the headers are read from the build tree.
target_sources(lense PRIVATE "${PROJECT_SOURCE_DIR}/src/Run.cpp")
target_include_directories(lense PRIVATE "${PROJECT_BINARY_DIR}")
target_compile_definitions(lense PRIVATE
  "LENSE_HEADER_ROOT=\"${LENSE_WASM_HEADERS}\"")
target_link_options(lense PRIVATE ${common}
  -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1)
add_dependencies(lense lense_headers)

add_executable(lense_web "${PROJECT_SOURCE_DIR}/src/Wasm.cpp"
                         "${PROJECT_SOURCE_DIR}/src/Run.cpp")
set_target_properties(lense_web PROPERTIES OUTPUT_NAME lense-web
                                           LINK_DEPENDS "${stamp}")
target_include_directories(lense_web PRIVATE "${PROJECT_BINARY_DIR}")
target_compile_definitions(lense_web PRIVATE "LENSE_HEADER_ROOT=\"/lense\"")
target_link_libraries(lense_web PRIVATE lense_core)
target_link_options(lense_web PRIVATE ${common}
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLense
  -sENVIRONMENT=web,worker,node
  -sEXPORTED_FUNCTIONS=_lense_analyze,_free
  -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString
  "SHELL:--preload-file ${LENSE_WASM_HEADERS}@/lense")
add_dependencies(lense_web lense_headers)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/lense-web.js"
  "${CMAKE_CURRENT_BINARY_DIR}/lense-web.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/lense-web.data"
  DESTINATION share/lense/web)

# Lets scripts run lense.js like a native binary.
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/lense-node"
  CONTENT "#!/bin/sh\nexec \"${CMAKE_CROSSCOMPILING_EMULATOR}\" \"$<TARGET_FILE:lense>\" \"$@\"\n"
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
                   GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
