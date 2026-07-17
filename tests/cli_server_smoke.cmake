if(NOT DEFINED TSM_EXECUTABLE OR NOT EXISTS "${TSM_EXECUTABLE}")
    message(FATAL_ERROR "TSM_EXECUTABLE does not point to the built application")
endif()
if(NOT DEFINED TSM_REQUESTS OR NOT EXISTS "${TSM_REQUESTS}")
    message(FATAL_ERROR "TSM_REQUESTS does not point to the NDJSON fixture")
endif()

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli serve --no-config --no-sound --quiet
    INPUT_FILE "${TSM_REQUESTS}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 20
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
        "CLI server exited with ${exit_code}\nstdout:\n${output}\nstderr:\n${error_output}")
endif()

string(REPLACE "\r\n" "\n" output "${output}")
string(REGEX REPLACE "\n$" "" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
list(LENGTH lines line_count)
if(NOT line_count EQUAL 5)
    message(FATAL_ERROR "Expected 5 NDJSON records, received ${line_count}:\n${output}")
endif()

list(GET lines 0 ready_line)
string(JSON ready_event ERROR_VARIABLE json_error GET "${ready_line}" event)
if(json_error OR NOT ready_event STREQUAL "ready")
    message(FATAL_ERROR "First record is not a valid ready event: ${ready_line}")
endif()
string(JSON ready_schema ERROR_VARIABLE json_error GET "${ready_line}" schemaVersion)
string(JSON ready_api ERROR_VARIABLE json_error GET "${ready_line}" apiVersion)
string(JSON ready_data_type ERROR_VARIABLE json_error TYPE "${ready_line}" data)
if(json_error OR NOT ready_schema EQUAL 1 OR NOT ready_api STREQUAL "2.0" OR
   NOT ready_data_type STREQUAL "OBJECT")
    message(FATAL_ERROR "Ready event violates the versioned contract: ${ready_line}")
endif()

set(expected_ids ping mixer status shutdown)
foreach(index RANGE 1 4)
    list(GET lines ${index} response_line)
    string(JSON response_ok ERROR_VARIABLE json_error GET "${response_line}" ok)
    if(json_error OR NOT response_ok)
        message(FATAL_ERROR "Invalid or failed response: ${response_line}")
    endif()
    string(JSON response_id ERROR_VARIABLE json_error GET "${response_line}" id)
    if(json_error)
        message(FATAL_ERROR "Response has no valid ID: ${response_line}")
    endif()
    math(EXPR expected_index "${index} - 1")
    list(GET expected_ids ${expected_index} expected_id)
    if(NOT response_id STREQUAL expected_id)
        message(FATAL_ERROR "Expected response '${expected_id}', received '${response_id}'")
    endif()
endforeach()

list(GET lines 3 status_line)
if(NOT status_line MATCHES "\"master\":0\\.75" OR
   NOT status_line MATCHES "\"music\":0\\.6")
    message(FATAL_ERROR "Persistent mixer state was not preserved: ${status_line}")
endif()
