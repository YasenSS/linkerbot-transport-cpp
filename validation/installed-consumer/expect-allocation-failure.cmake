if(NOT DEFINED PROGRAM)
    message(FATAL_ERROR "PROGRAM is required")
endif()
execute_process(COMMAND "${PROGRAM}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 10)
if(NOT "${result}" STREQUAL "1")
    message(FATAL_ERROR "Expected handled allocation failure (exit 1), got ${result}: ${error}")
endif()
if(NOT error MATCHES "Installed consumer failed:")
    message(FATAL_ERROR "Missing allocation-failure diagnostic: ${error}")
endif()
if(output MATCHES "behavior passed")
    message(FATAL_ERROR "Consumer reported success after allocation failure")
endif()
