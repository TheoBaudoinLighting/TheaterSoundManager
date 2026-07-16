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
    VERSION 1.0.0
    DESCRIPTION "Desktop audio control for theaters and live events"
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
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(CTest)
include(Dependencies)
include(Runtime)

set(TSM_CORE_SOURCES
    src/tsm_announcement_manager.cpp
    src/tsm_audio_manager.cpp
    src/tsm_bluetooth_server.cpp
    src/tsm_config.cpp
    src/tsm_fmod_wrapper.cpp
    src/tsm_playlist_manager.cpp
    src/tsm_ui_manager.cpp
)

set(TSM_PUBLIC_HEADERS
    src/tsm_announcement_manager.h
    src/tsm_audio_manager.h
    src/tsm_bluetooth_server.h
    src/tsm_config.h
    src/tsm_fmod_wrapper.h
    src/tsm_logger.h
    src/tsm_playlist_manager.h
    src/tsm_transition_logic.h
    src/tsm_ui_manager.h
)

add_library(tsm_core STATIC ${TSM_CORE_SOURCES} ${TSM_PUBLIC_HEADERS})
add_library(TSM::Core ALIAS tsm_core)
target_include_directories(tsm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tsm_core
    PUBLIC
        FMOD::FMOD
        SDL2::SDL2
        Threads::Threads
    PRIVATE
        ImGui::ImGui
        nlohmann_json::nlohmann_json
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
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(BUILD_TESTING)
    add_executable(TheaterSoundManagerTests
        tests/main.cpp
        tests/tsm_annoucement_manager_tests.cpp
        tests/tsm_audio_manager_logic_tests.cpp
        tests/tsm_config_tests.cpp
        tests/tsm_playlist_manager_test.cpp
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
endif()

install(TARGETS TheaterSoundManager RUNTIME DESTINATION ".")
install(FILES
    "$<TARGET_FILE:SDL2::SDL2>"
    "$<TARGET_FILE:FMOD::FMOD>"
    DESTINATION "."
)
install(DIRECTORY config/ DESTINATION config)
install(DIRECTORY "${TSM_THIRD_PARTY_LICENSE_DIR}/" DESTINATION licenses)
install(FILES external/README.md DESTINATION licenses RENAME FMOD-SDK.txt)
if(TSM_INSTALL_ASSETS)
    install(DIRECTORY assets/ DESTINATION assets)
endif()
install(FILES README.md LICENSE DESTINATION ".")

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME
    "TheaterSoundManager-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "TheaterSoundManager")
include(CPack)
