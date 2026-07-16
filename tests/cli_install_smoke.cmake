foreach(required_variable
        TSM_BUILD_DIR
        TSM_CONFIGURATION
        TSM_INSTALL_DIR
        TSM_EXTERNAL_CWD)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TSM_INSTALL_DIR}" "${TSM_EXTERNAL_CWD}")
file(MAKE_DIRECTORY "${TSM_EXTERNAL_CWD}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${TSM_BUILD_DIR}"
        --config "${TSM_CONFIGURATION}"
        --prefix "${TSM_INSTALL_DIR}"
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
    RESULT_VARIABLE install_exit
    TIMEOUT 45
)
if(NOT install_exit EQUAL 0)
    message(FATAL_ERROR
        "Installation failed (${install_exit})\n${install_output}\n${install_error}")
endif()

set(installed_executable "${TSM_INSTALL_DIR}/TheaterSoundManager.exe")
set(installed_config "${TSM_INSTALL_DIR}/config/tsm_config.json")
string(TOLOWER "${TSM_CONFIGURATION}" configuration_lower)
if(configuration_lower STREQUAL "debug")
    set(sdl_runtime SDL2d.dll)
    set(fmod_runtime fmodL.dll)
else()
    set(sdl_runtime SDL2.dll)
    set(fmod_runtime fmod.dll)
endif()

set(required_payload
    TheaterSoundManager.exe
    ${sdl_runtime}
    ${fmod_runtime}
    msvcp140.dll
    vcruntime140.dll
    config/tsm_config.json
    config/tsm_config.cinema.example.json
    config/README.md
    assets/fonts/Jost-Regular.ttf
    docs/CLI.md
    docs/CINEMA.md
    examples/cli_client.py
    external/README.md
    licenses/ImGui.txt
    licenses/Jost.txt
    licenses/nlohmann-json.txt
    licenses/SDL2.txt
    licenses/spdlog.txt
    README.md
    LICENSE
)
set(missing_payload)
foreach(relative_path IN LISTS required_payload)
    if(NOT EXISTS "${TSM_INSTALL_DIR}/${relative_path}")
        list(APPEND missing_payload "${relative_path}")
    endif()
endforeach()
if(missing_payload)
    list(JOIN missing_payload "\n  - " missing_payload_text)
    message(FATAL_ERROR "Installation is missing required payload:\n  - ${missing_payload_text}")
endif()

execute_process(
    COMMAND "${installed_executable}" --cli --version
    WORKING_DIRECTORY "${TSM_EXTERNAL_CWD}"
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
    RESULT_VARIABLE version_exit
    TIMEOUT 10
)
if(NOT version_exit EQUAL 0 OR NOT version_error STREQUAL "" OR
   NOT version_output MATCHES "\"cliApiVersion\":\"1\\.0\"")
    message(FATAL_ERROR
        "Installed version smoke failed (${version_exit})\n${version_output}\n${version_error}")
endif()

execute_process(
    COMMAND "${installed_executable}" --cli status --no-sound --quiet
    WORKING_DIRECTORY "${TSM_EXTERNAL_CWD}"
    OUTPUT_VARIABLE status_output
    ERROR_VARIABLE status_error
    RESULT_VARIABLE status_exit
    TIMEOUT 20
)
if(NOT status_exit EQUAL 0 OR NOT status_error STREQUAL "")
    message(FATAL_ERROR
        "Installed status smoke failed (${status_exit})\n${status_output}\n${status_error}")
endif()
string(STRIP "${status_output}" status_output)
string(JSON status_ok ERROR_VARIABLE status_json_error GET "${status_output}" ok)
string(JSON config_path ERROR_VARIABLE config_json_error GET
    "${status_output}" data runtime configPath)
string(JSON resource_root ERROR_VARIABLE resource_json_error GET
    "${status_output}" data runtime resourceRoot)
string(JSON failure_count ERROR_VARIABLE failures_json_error LENGTH
    "${status_output}" data runtime load failedAssets)
string(JSON cinema_safe ERROR_VARIABLE cinema_json_error GET
    "${status_output}" data cinema safeToPlay)
if(status_json_error OR config_json_error OR resource_json_error OR
   failures_json_error OR cinema_json_error OR NOT status_ok OR
   NOT failure_count EQUAL 0 OR NOT cinema_safe)
    message(FATAL_ERROR "Installed status violates its contract: ${status_output}")
endif()

cmake_path(SET expected_config NORMALIZE "${installed_config}")
cmake_path(SET actual_config NORMALIZE "${config_path}")
if(NOT actual_config STREQUAL expected_config)
    message(FATAL_ERROR
        "Installed config was not resolved beside the executable:\n"
        "expected=${expected_config}\nactual=${actual_config}")
endif()

cmake_path(SET expected_resource_root NORMALIZE "${TSM_INSTALL_DIR}")
cmake_path(SET actual_resource_root NORMALIZE "${resource_root}")
if(NOT actual_resource_root STREQUAL expected_resource_root)
    message(FATAL_ERROR
        "Installed resource root was not resolved beside the executable:\n"
        "expected=${expected_resource_root}\nactual=${actual_resource_root}")
endif()

if(TSM_EXPECT_EMPTY_CONFIG)
    string(JSON playlist_count ERROR_VARIABLE playlist_json_error GET
        "${status_output}" data playlists)
    string(JSON sound_count ERROR_VARIABLE sound_json_error GET
        "${status_output}" data sounds)
    string(JSON announcement_count ERROR_VARIABLE announcement_json_error GET
        "${status_output}" data runtime load announcements)
    if(playlist_json_error OR sound_json_error OR announcement_json_error OR
       NOT playlist_count EQUAL 0 OR NOT sound_count EQUAL 0 OR
       NOT announcement_count EQUAL 0)
        message(FATAL_ERROR
            "Asset-free install did not use its empty production config: ${status_output}")
    endif()

    foreach(media_directory musics annonces announce sfx wedding)
        if(EXISTS "${TSM_INSTALL_DIR}/assets/${media_directory}")
            message(FATAL_ERROR
                "Asset-free install unexpectedly contains assets/${media_directory}")
        endif()
    endforeach()
endif()

if(DEFINED TSM_PYTHON_EXECUTABLE AND
   NOT TSM_PYTHON_EXECUTABLE STREQUAL "" AND
   EXISTS "${TSM_PYTHON_EXECUTABLE}")
    execute_process(
        COMMAND "${TSM_PYTHON_EXECUTABLE}" -B
            "${TSM_INSTALL_DIR}/examples/cli_client.py"
            "${installed_executable}"
        WORKING_DIRECTORY "${TSM_EXTERNAL_CWD}"
        OUTPUT_VARIABLE client_output
        ERROR_VARIABLE client_error
        RESULT_VARIABLE client_exit
        TIMEOUT 30
    )
    if(NOT client_exit EQUAL 0 OR NOT client_error STREQUAL "" OR
       NOT client_output MATCHES "'master': 0\\.8")
        message(FATAL_ERROR
            "Installed Python client smoke failed (${client_exit})\n"
            "${client_output}\n${client_error}")
    endif()
endif()

file(GLOB_RECURSE external_cwd_entries LIST_DIRECTORIES TRUE
    "${TSM_EXTERNAL_CWD}/*")
if(external_cwd_entries)
    list(JOIN external_cwd_entries "\n  - " external_cwd_text)
    message(FATAL_ERROR
        "Installed CLI wrote runtime artifacts to its caller directory:\n"
        "  - ${external_cwd_text}")
endif()
