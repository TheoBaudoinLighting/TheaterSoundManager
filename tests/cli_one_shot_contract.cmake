if(NOT DEFINED TSM_EXECUTABLE OR NOT EXISTS "${TSM_EXECUTABLE}")
    message(FATAL_ERROR "TSM_EXECUTABLE does not point to the built application")
endif()
if(NOT DEFINED TSM_TEMP_DIR)
    message(FATAL_ERROR "TSM_TEMP_DIR is required")
endif()

file(MAKE_DIRECTORY "${TSM_TEMP_DIR}")

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli --quiet --request
        "{\"id\":\"echo-me\",\"command\":\"system.ping\",\"params\":{}}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 0 OR NOT error_output STREQUAL "")
    message(FATAL_ERROR "Raw request failed (${exit_code}): ${output}\n${error_output}")
endif()
string(STRIP "${output}" output)
string(JSON response_id GET "${output}" id)
string(JSON response_ok GET "${output}" ok)
if(NOT response_ok OR NOT response_id STREQUAL "echo-me")
    message(FATAL_ERROR "Raw request ID was not echoed: ${output}")
endif()

set(missing_config "${TSM_TEMP_DIR}/intentionally-missing.json")
file(REMOVE "${missing_config}")
execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli --quiet --no-sound
        --config "${missing_config}" --request
        "{\"id\":17,\"command\":\"unknown.command\",\"params\":{}}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 2 OR NOT error_output STREQUAL "")
    message(FATAL_ERROR "Unknown raw command should fail before config load: ${exit_code}\n${output}\n${error_output}")
endif()
string(STRIP "${output}" output)
string(JSON error_code GET "${output}" error code)
if(NOT error_code STREQUAL "unknown_command")
    message(FATAL_ERROR "Unknown raw command was misclassified: ${output}")
endif()

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli mixer set --master 0.8 --no-sound --quiet
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 5)
    message(FATAL_ERROR "One-shot state mutation should exit 5, got ${exit_code}: ${output}")
endif()
string(STRIP "${output}" output)
string(JSON error_code GET "${output}" error code)
if(NOT error_code STREQUAL "persistent_host_required")
    message(FATAL_ERROR "Unexpected state mutation error: ${output}")
endif()

foreach(library_command IN ITEMS stop next)
    execute_process(
        COMMAND "${TSM_EXECUTABLE}" --cli library "${library_command}"
            --no-sound --quiet
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
        RESULT_VARIABLE exit_code
        TIMEOUT 10
    )
    if(NOT exit_code EQUAL 5 OR NOT error_output STREQUAL "")
        message(FATAL_ERROR
            "One-shot library ${library_command} should require serve, got ${exit_code}: ${output}\n${error_output}")
    endif()
    string(STRIP "${output}" output)
    string(JSON error_code GET "${output}" error code)
    if(NOT error_code STREQUAL "persistent_host_required")
        message(FATAL_ERROR
            "Unexpected library ${library_command} error: ${output}")
    endif()
endforeach()

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli playlist options playlist_PreShow
        --no-sound --quiet
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 0 OR NOT error_output STREQUAL "")
    message(FATAL_ERROR "One-shot playlist option read failed (${exit_code}): ${output}\n${error_output}")
endif()

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli playlist options playlist_PreShow
        --loop=false --no-sound --quiet
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 5)
    message(FATAL_ERROR "One-shot playlist option write should exit 5, got ${exit_code}: ${output}")
endif()

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli loudness target --no-sound --quiet
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 0 OR NOT error_output STREQUAL "")
    message(FATAL_ERROR "One-shot loudness target read failed (${exit_code}): ${output}\n${error_output}")
endif()

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli loudness target --value -18
        --no-sound --quiet
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 5)
    message(FATAL_ERROR "One-shot loudness target write should exit 5, got ${exit_code}: ${output}")
endif()

set(blocking_log_parent "${TSM_TEMP_DIR}/not-a-directory")
file(WRITE "${blocking_log_parent}" "regular file")
execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli system ping --quiet
        --log-file "${blocking_log_parent}/cli.log"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 8)
    message(FATAL_ERROR "Invalid log path should exit 8, got ${exit_code}: ${output}")
endif()
string(STRIP "${output}" output)
string(JSON error_code GET "${output}" error code)
if(NOT error_code STREQUAL "log_initialization_failed")
    message(FATAL_ERROR "Unexpected log initialization error: ${output}")
endif()

set(invalid_config "${TSM_TEMP_DIR}/invalid.json")
file(WRITE "${invalid_config}" "{invalid")
execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli status --config "${invalid_config}"
        --no-sound --quiet
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)
if(NOT exit_code EQUAL 6)
    message(FATAL_ERROR "Invalid config should exit 6, got ${exit_code}: ${output}")
endif()
string(STRIP "${output}" output)
string(JSON error_code GET "${output}" error code)
if(NOT error_code STREQUAL "runtime_configuration_failed")
    message(FATAL_ERROR "Unexpected invalid config classification: ${output}")
endif()
