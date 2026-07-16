cmake_minimum_required(VERSION 3.25)

cmake_path(SET TSM_EXPECTED_BINARY_DIR NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/build")
cmake_path(SET TSM_ACTUAL_BINARY_DIR NORMALIZE "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT TSM_ACTUAL_BINARY_DIR STREQUAL TSM_EXPECTED_BINARY_DIR)
    message(FATAL_ERROR
        "Theater Sound Manager uses a single disposable build directory.\n"
        "Configure with: cmake --preset default\n"
        "Expected: ${TSM_EXPECTED_BINARY_DIR}\n"
        "Received: ${TSM_ACTUAL_BINARY_DIR}")
endif()

project(
    TheaterSoundManager
    VERSION 1.1.2
    DESCRIPTION "Production audio automation and safety integration for cinemas"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Theater Sound Manager currently supports Windows only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Theater Sound Manager and FMOD require a 64-bit build.")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
include(InstallRequiredSystemLibraries)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(CTest)
include(Dependencies)
include(Runtime)

set(TSM_CORE_SOURCES
    src/tsm_announcement_manager.cpp
    src/tsm_atomic_file.cpp
    src/tsm_audio_manager.cpp
    src/tsm_bluetooth_server.cpp
    src/tsm_cinema_manager.cpp
    src/tsm_cinema_schedule.cpp
    src/tsm_cli_commands.cpp
    src/tsm_cli_host.cpp
    src/tsm_cli_parser.cpp
    src/tsm_config.cpp
    src/tsm_fmod_wrapper.cpp
    src/tsm_mixer.cpp
    src/tsm_playlist_manager.cpp
    src/tsm_runtime.cpp
    src/tsm_safety.cpp
    src/tsm_ui_manager.cpp
)

set(TSM_PUBLIC_HEADERS
    src/tsm_announcement_manager.h
    src/tsm_atomic_file.h
    src/tsm_audio_manager.h
    src/tsm_bluetooth_server.h
    src/tsm_cinema_manager.h
    src/tsm_cinema_schedule.h
    src/tsm_cli.h
    src/tsm_config.h
    src/tsm_fmod_wrapper.h
    src/tsm_logger.h
    src/tsm_mixer.h
    src/tsm_playlist_manager.h
    src/tsm_runtime.h
    src/tsm_safety.h
    src/tsm_transition_logic.h
    src/tsm_ui_manager.h
)

add_library(tsm_core STATIC ${TSM_CORE_SOURCES} ${TSM_PUBLIC_HEADERS})
add_library(TSM::Core ALIAS tsm_core)
target_include_directories(tsm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(tsm_core PUBLIC TSM_VERSION="${PROJECT_VERSION}")
target_link_libraries(tsm_core
    PUBLIC
        FMOD::FMOD
        SDL2::SDL2
        Threads::Threads
        nlohmann_json::nlohmann_json
    PRIVATE
        ImGui::ImGui
        spdlog::spdlog_header_only
        OpenGL::GL
        bthprops
        ole32
        shell32
        uuid
        ws2_32
)
tsm_configure_target(tsm_core)
set_target_properties(tsm_core PROPERTIES FOLDER "TSM")

add_executable(TheaterSoundManager src/tsm_main.cpp)
target_link_libraries(TheaterSoundManager
    PRIVATE
        TSM::Core
        SDL2::SDL2main
        spdlog::spdlog_header_only
)
tsm_configure_target(TheaterSoundManager)
tsm_stage_runtime(TheaterSoundManager)
set_target_properties(TheaterSoundManager PROPERTIES
    FOLDER "TSM"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
)

if(BUILD_TESTING)
    add_executable(TheaterSoundManagerTests
        tests/main.cpp
        tests/tsm_annoucement_manager_tests.cpp
        tests/tsm_audio_manager_logic_tests.cpp
        tests/tsm_cinema_manager_tests.cpp
        tests/tsm_cinema_schedule_tests.cpp
        tests/tsm_cli_tests.cpp
        tests/tsm_config_tests.cpp
        tests/tsm_playlist_manager_test.cpp
        tests/tsm_playlist_resume_tests.cpp
        tests/tsm_safety_tests.cpp
        tests/tsm_transition_logic_tests.cpp
        tests/tsm_ui_manager_tests.cpp
    )
    target_link_libraries(TheaterSoundManagerTests
        PRIVATE
            TSM::Core
            GTest::gtest
    )
    target_compile_definitions(TheaterSoundManagerTests PRIVATE SDL_MAIN_HANDLED)
    target_precompile_headers(TheaterSoundManagerTests PRIVATE tests/pch.h)
    tsm_configure_target(TheaterSoundManagerTests)
    tsm_stage_runtime(TheaterSoundManagerTests)
    set_target_properties(TheaterSoundManagerTests PROPERTIES FOLDER "Tests")

    include(GoogleTest)
    gtest_discover_tests(TheaterSoundManagerTests
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:TheaterSoundManagerTests>"
    )

    add_test(
        NAME CliExecutableVersion
        COMMAND "$<TARGET_FILE:TheaterSoundManager>" --cli --version
    )
    set_tests_properties(CliExecutableVersion PROPERTIES
        LABELS "cli;smoke"
        PASS_REGULAR_EXPRESSION "\"cliApiVersion\":\"1.0\""
        TIMEOUT 10
    )

    add_test(
        NAME CliServerSmoke
        COMMAND "${CMAKE_COMMAND}"
            "-DTSM_EXECUTABLE=$<TARGET_FILE:TheaterSoundManager>"
            "-DTSM_REQUESTS=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cli_server_requests.ndjson"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_server_smoke.cmake"
    )
    set_tests_properties(CliServerSmoke PROPERTIES
        LABELS "cli;integration"
        TIMEOUT 30
    )

    add_test(
        NAME CliServerContract
        COMMAND "${CMAKE_COMMAND}"
            "-DTSM_EXECUTABLE=$<TARGET_FILE:TheaterSoundManager>"
            "-DTSM_TEMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-contract"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_server_contract.cmake"
    )
    set_tests_properties(CliServerContract PROPERTIES
        LABELS "cli;integration"
        TIMEOUT 30
    )

    add_test(
        NAME CliOneShotContract
        COMMAND "${CMAKE_COMMAND}"
            "-DTSM_EXECUTABLE=$<TARGET_FILE:TheaterSoundManager>"
            "-DTSM_TEMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-one-shot"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_one_shot_contract.cmake"
    )
    set_tests_properties(CliOneShotContract PROPERTIES
        LABELS "cli;integration"
        TIMEOUT 30
    )

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME CliPythonClientSmoke
            COMMAND "${Python3_EXECUTABLE}"
                -B
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/cli_client.py"
                "$<TARGET_FILE:TheaterSoundManager>"
        )
        set_tests_properties(CliPythonClientSmoke PROPERTIES
            LABELS "cli;integration;python"
            PASS_REGULAR_EXPRESSION "'mixer'"
            TIMEOUT 30
        )

        add_test(
            NAME CliPythonClientContract
            COMMAND "${Python3_EXECUTABLE}"
                -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_python_client_tests.py"
                "$<TARGET_FILE:TheaterSoundManager>"
        )
        set_tests_properties(CliPythonClientContract PROPERTIES
            LABELS "cli;integration;python"
            PASS_REGULAR_EXPRESSION "python client contract: ok"
            TIMEOUT 30
        )
    endif()

    add_test(
        NAME CliInstallSmoke
        COMMAND "${CMAKE_COMMAND}"
            "-DTSM_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DTSM_CONFIGURATION=$<CONFIG>"
            "-DTSM_INSTALL_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-smoke/$<CONFIG>"
            "-DTSM_EXTERNAL_CWD=${CMAKE_CURRENT_BINARY_DIR}/install-smoke-cwd/$<CONFIG>"
            "-DTSM_EXPECT_EMPTY_CONFIG=$<NOT:$<BOOL:${TSM_INSTALL_ASSETS}>>"
            "-DTSM_PYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_install_smoke.cmake"
    )
    set_tests_properties(CliInstallSmoke PROPERTIES
        LABELS "cli;deployment;integration"
        TIMEOUT 60
    )
endif()

install(TARGETS TheaterSoundManager RUNTIME DESTINATION ".")
install(FILES
    "$<TARGET_FILE:SDL2::SDL2>"
    "$<TARGET_FILE:FMOD::FMOD>"
    DESTINATION "."
)
install(FILES
    config/README.md
    config/tsm_config.cinema.example.json
    DESTINATION config)
if(TSM_INSTALL_ASSETS)
    install(FILES config/tsm_config.json DESTINATION config)
else()
    install(FILES config/tsm_config.empty.json
        DESTINATION config
        RENAME tsm_config.json)
endif()
install(DIRECTORY docs/ DESTINATION docs)
install(DIRECTORY examples/ DESTINATION examples)
install(FILES external/README.md DESTINATION external)
install(DIRECTORY "${TSM_THIRD_PARTY_LICENSE_DIR}/" DESTINATION licenses)
install(FILES assets/fonts/Jost-Regular.ttf DESTINATION assets/fonts)
install(FILES assets/fonts/OFL.txt DESTINATION licenses RENAME Jost.txt)
if(TSM_INSTALL_ASSETS)
    install(DIRECTORY assets/ DESTINATION assets PATTERN "fonts" EXCLUDE)
endif()
install(FILES README.md LICENSE DESTINATION ".")

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME
    "TheaterSoundManager-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "TheaterSoundManager")
include(CPack)
