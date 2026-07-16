if(NOT DEFINED TSM_EXECUTABLE OR NOT EXISTS "${TSM_EXECUTABLE}")
    message(FATAL_ERROR "TSM_EXECUTABLE does not point to the built application")
endif()
if(NOT DEFINED TSM_TEMP_DIR)
    message(FATAL_ERROR "TSM_TEMP_DIR is required")
endif()

file(MAKE_DIRECTORY "${TSM_TEMP_DIR}")
set(requests "${TSM_TEMP_DIR}/requests.ndjson")
string(REPEAT "x" 1048577 oversized_request)
file(WRITE "${requests}" "not json\n")
file(APPEND "${requests}" "{\"id\":true,\"command\":\"system.ping\",\"params\":{}}\n")
file(APPEND "${requests}" "{\"id\":{\"bad\":1},\"command\":\"system.ping\",\"params\":{}}\n")
file(APPEND "${requests}" "{\"id\":\"params\",\"command\":\"system.ping\",\"params\":[]}\n")
file(APPEND "${requests}" "{\"id\":\"unknown\",\"command\":\"missing.command\",\"params\":{}}\n")
file(APPEND "${requests}" "${oversized_request}\n")
file(APPEND "${requests}" "{\"id\":\"shutdown\",\"command\":\"system.shutdown\",\"params\":{}}\n")

execute_process(
    COMMAND "${TSM_EXECUTABLE}" --cli serve --no-config --no-sound --quiet
    INPUT_FILE "${requests}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 20
)
if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
        "CLI server exited with ${exit_code}\nstdout:\n${output}\nstderr:\n${error_output}")
endif()
if(NOT error_output STREQUAL "")
    message(FATAL_ERROR "Quiet server wrote to stderr: ${error_output}")
endif()

string(REPLACE "\r\n" "\n" output "${output}")
string(REGEX REPLACE "\n$" "" output "${output}")
string(REPLACE ";" "\\;" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
list(LENGTH lines line_count)
if(NOT line_count EQUAL 8)
    message(FATAL_ERROR "Expected 8 NDJSON records, received ${line_count}:\n${output}")
endif()

list(GET lines 0 ready)
string(JSON ready_event GET "${ready}" event)
if(NOT ready_event STREQUAL "ready")
    message(FATAL_ERROR "Missing ready event: ${ready}")
endif()

list(GET lines 1 invalid_json)
string(JSON invalid_json_code GET "${invalid_json}" error code)
if(NOT invalid_json_code STREQUAL "invalid_json")
    message(FATAL_ERROR "Expected invalid_json: ${invalid_json}")
endif()

list(GET lines 2 boolean_id)
string(JSON boolean_id_type TYPE "${boolean_id}" id)
string(JSON boolean_id_ok GET "${boolean_id}" ok)
if(NOT boolean_id_type STREQUAL "BOOLEAN" OR NOT boolean_id_ok)
    message(FATAL_ERROR "Boolean ID was not echoed: ${boolean_id}")
endif()

set(expected_errors invalid_request_id invalid_params unknown_command request_too_large)
foreach(index RANGE 3 6)
    list(GET lines ${index} response)
    string(JSON response_ok GET "${response}" ok)
    if(response_ok)
        message(FATAL_ERROR "Expected an error response: ${response}")
    endif()
    string(JSON response_code GET "${response}" error code)
    math(EXPR expected_index "${index} - 3")
    list(GET expected_errors ${expected_index} expected_code)
    if(NOT response_code STREQUAL expected_code)
        message(FATAL_ERROR "Expected ${expected_code}, received: ${response}")
    endif()
endforeach()

list(GET lines 3 invalid_id)
string(JSON invalid_id_type TYPE "${invalid_id}" id)
if(NOT invalid_id_type STREQUAL "NULL")
    message(FATAL_ERROR "Invalid structured ID must not be echoed: ${invalid_id}")
endif()

list(GET lines 7 shutdown)
string(JSON shutdown_ok GET "${shutdown}" ok)
string(JSON shutdown_id GET "${shutdown}" id)
if(NOT shutdown_ok OR NOT shutdown_id STREQUAL "shutdown")
    message(FATAL_ERROR "Invalid shutdown response: ${shutdown}")
endif()
