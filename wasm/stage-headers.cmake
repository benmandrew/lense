# Copies the headers that the WebAssembly builds carry into OUT:
#
#   OUT/clang/include  clang's builtin headers, from RESOURCE
#   OUT/include        libc++ and musl, from the Emscripten SYSROOT
#
# Emscripten's own APIs, graphics libraries and SIMD shims are left out: lense
# never targets Emscripten, and clang's builtin headers have the intrinsics.
# So are clang's headers for targets the browser does not offer (GPU offload,
# OpenCL, HLSL, PowerPC, s390, Hexagon, LoongArch, MIPS, z/OS): 4.25 MB.
# With PYTHON set, comments are stripped as well (strip-comments.py).
# libc++ is switched from Emscripten's ABI version 2 to the usual version 1,
# so that standard library types have the layout they have on macOS and on
# Linux with libc++.
#
#   cmake -DRESOURCE=<dir> -DSYSROOT=<dir> -DOUT=<dir> [-DPYTHON=<python3>]
#         -P stage-headers.cmake

foreach(var RESOURCE SYSROOT OUT)
  if(NOT ${var})
    message(FATAL_ERROR "stage-headers.cmake: set ${var}")
  endif()
endforeach()

set(perms
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
  DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

file(REMOVE_RECURSE "${OUT}")
file(COPY "${RESOURCE}/include" DESTINATION "${OUT}/clang" ${perms})
set(unused
  __clang_cuda_* __clang_hip_* __clang_gpu_* __clang_spirv_* *_wrappers hlsl*
  opencl-c*.h hexagon_*.h hvx_hexagon_protos.h altivec.h htm*intrin.h
  vecintrin.h s390intrin.h larchintrin.h lasxintrin.h lsxintrin.h msa.h
  gpuintrin.h amdgpuintrin.h nvptxintrin.h spirvintrin.h)
list(TRANSFORM unused PREPEND "${OUT}/clang/include/")
file(GLOB unused ${unused})
file(REMOVE_RECURSE ${unused})

set(skip
  AL EGL GL GLES GLES2 GLES3 GLFW KHR SDL X11 fakesdl webgl compat
  emscripten emscripten.h wasm_simd128.h sanitizer profile mach-o
  mimalloc mimalloc.h mimalloc-new-delete.h mimalloc-override.h
  mimalloc-stats.h omp.h ompx.h
  libunwind.h libunwind.modulemap __libunwind_config.h
  unwind.h unwind_arm_ehabi.h unwind_itanium.h unwind_wasm.h)
file(GLOB entries RELATIVE "${SYSROOT}/include" "${SYSROOT}/include/*")
foreach(e IN LISTS entries)
  if(NOT e IN_LIST skip)
    file(COPY "${SYSROOT}/include/${e}" DESTINATION "${OUT}/include" ${perms})
  endif()
endforeach()

set(site "${OUT}/include/c++/v1/__config_site")
file(READ "${site}" text)
string(REGEX REPLACE "#define _LIBCPP_ABI_VERSION [0-9]+"
       "#define _LIBCPP_ABI_VERSION 1" text "${text}")
string(REGEX REPLACE "#define _LIBCPP_ABI_NAMESPACE __[0-9]+"
       "#define _LIBCPP_ABI_NAMESPACE __1" text "${text}")
if(NOT text MATCHES "_LIBCPP_ABI_VERSION 1\n")
  message(FATAL_ERROR "could not set the libc++ ABI version in ${site}")
endif()
file(WRITE "${site}" "${text}")

if(PYTHON)
  execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/strip-comments.py" "${OUT}"
    COMMAND_ERROR_IS_FATAL ANY)
endif()
