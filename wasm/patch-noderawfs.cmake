# Works around a bug in Emscripten 6.0.9's NODERAWFS: fstat() on standard
# input or output reads stream.node.node_ops, but those streams have no node,
# so every program that calls fstat(0..2) (LLVM's raw_fd_ostream does) dies.
#
#   cmake -DJS=<lense.js> -P patch-noderawfs.cmake

file(READ "${JS}" text)
set(bad "stream.node.node_ops?.getattr")
set(good "stream.node?.node_ops?.getattr")
string(FIND "${text}" "${bad}" at)
if(at GREATER_EQUAL 0)
  string(REPLACE "${bad}" "${good}" text "${text}")
  file(WRITE "${JS}" "${text}")
else()
  string(FIND "${text}" "${good}" fixed)
  if(fixed LESS 0)
    message(FATAL_ERROR "patch-noderawfs.cmake: Emscripten's fstat changed; "
                        "check whether the workaround is still needed")
  endif()
endif()
