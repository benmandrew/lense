# Runs lense on one input and compares stdout with a golden file.
string(REPLACE "|" ";" FLAGS "${FLAGS}")
execute_process(COMMAND ${LENSE} ${FLAGS} ${INPUT}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL EXPECT_RC)
  message(FATAL_ERROR "lense exited ${rc}, expected ${EXPECT_RC}\n${err}")
endif()

if(DEFINED ENV{LENSE_UPDATE})
  file(WRITE "${EXPECTED}" "${out}")
  message(STATUS "Updated ${EXPECTED}")
  return()
endif()

if(NOT EXISTS "${EXPECTED}")
  message(FATAL_ERROR "Missing ${EXPECTED}; run with LENSE_UPDATE=1")
endif()
file(READ "${EXPECTED}" expected)
if(NOT out STREQUAL expected)
  file(WRITE "${ACTUAL}" "${out}")
  execute_process(COMMAND diff -u "${EXPECTED}" "${ACTUAL}")
  message(FATAL_ERROR "Output differs from ${EXPECTED}")
endif()
