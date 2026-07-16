include_guard(GLOBAL)

include(FetchContent)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Do not update pinned dependencies after their first download")

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl2
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.30.8.tar.gz
    URL_HASH SHA256=EA638D142EE2BF71E2896FBC87E2EAA5956D2C91322AA55CF41049382A6E7730
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sdl2)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.tar.gz
    URL_HASH SHA256=9962648C9B4F1A7BBC76FD8D9172555BAD1871FDB14FF4F842EF87949682CAA5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(spdlog)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    URL_HASH SHA256=0D8EF5AF7F9794E3263480193C491549B2BA6CC74BB018906202ADA498A79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/c71e4e8c7cb9b42b460bbaedfa4bc443f885b05b.tar.gz
    URL_HASH SHA256=A293AC087A78A01587E0F243052550D387A531E010B98877B96AC326B3C167EB
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)
add_library(tsm_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp"
)
add_library(ImGui::ImGui ALIAS tsm_imgui)
target_include_directories(tsm_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(tsm_imgui PUBLIC SDL2::SDL2 OpenGL::GL)
set_target_properties(tsm_imgui PROPERTIES FOLDER "Dependencies")

set(TSM_THIRD_PARTY_LICENSE_DIR "${CMAKE_BINARY_DIR}/licenses")
file(MAKE_DIRECTORY "${TSM_THIRD_PARTY_LICENSE_DIR}")
configure_file("${sdl2_SOURCE_DIR}/LICENSE.txt"
    "${TSM_THIRD_PARTY_LICENSE_DIR}/SDL2.txt" COPYONLY)
configure_file("${imgui_SOURCE_DIR}/LICENSE.txt"
    "${TSM_THIRD_PARTY_LICENSE_DIR}/ImGui.txt" COPYONLY)
configure_file("${spdlog_SOURCE_DIR}/LICENSE"
    "${TSM_THIRD_PARTY_LICENSE_DIR}/spdlog.txt" COPYONLY)
configure_file("${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
    "${TSM_THIRD_PARTY_LICENSE_DIR}/nlohmann-json.txt" COPYONLY)

set(TSM_FMOD_ROOT "${PROJECT_SOURCE_DIR}/external/fmod" CACHE PATH
    "FMOD Core API root containing inc/ and lib/x64/")
cmake_path(ABSOLUTE_PATH TSM_FMOD_ROOT
    BASE_DIRECTORY "${PROJECT_SOURCE_DIR}"
    NORMALIZE
    OUTPUT_VARIABLE TSM_FMOD_ROOT)

set(TSM_FMOD_REQUIRED_FILES
    "${TSM_FMOD_ROOT}/inc/fmod.hpp"
    "${TSM_FMOD_ROOT}/inc/fmod_errors.h"
    "${TSM_FMOD_ROOT}/lib/x64/fmod_vc.lib"
    "${TSM_FMOD_ROOT}/lib/x64/fmod.dll"
    "${TSM_FMOD_ROOT}/lib/x64/fmodL_vc.lib"
    "${TSM_FMOD_ROOT}/lib/x64/fmodL.dll"
)
set(TSM_FMOD_MISSING_FILES "")
foreach(required_file IN LISTS TSM_FMOD_REQUIRED_FILES)
    if(NOT EXISTS "${required_file}")
        list(APPEND TSM_FMOD_MISSING_FILES "${required_file}")
    endif()
endforeach()
if(TSM_FMOD_MISSING_FILES)
    list(JOIN TSM_FMOD_MISSING_FILES "\n  - " TSM_FMOD_MISSING_TEXT)
    message(FATAL_ERROR
        "FMOD Core API is incomplete. Missing:\n  - ${TSM_FMOD_MISSING_TEXT}\n"
        "Set TSM_FMOD_ROOT to the FMOD api/core directory or follow external/README.md.")
endif()

add_library(FMOD::FMOD SHARED IMPORTED GLOBAL)
set_target_properties(FMOD::FMOD PROPERTIES
    IMPORTED_CONFIGURATIONS "Debug;Release"
    IMPORTED_IMPLIB_DEBUG "${TSM_FMOD_ROOT}/lib/x64/fmodL_vc.lib"
    IMPORTED_LOCATION_DEBUG "${TSM_FMOD_ROOT}/lib/x64/fmodL.dll"
    IMPORTED_IMPLIB_RELEASE "${TSM_FMOD_ROOT}/lib/x64/fmod_vc.lib"
    IMPORTED_LOCATION_RELEASE "${TSM_FMOD_ROOT}/lib/x64/fmod.dll"
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    INTERFACE_INCLUDE_DIRECTORIES "${TSM_FMOD_ROOT}/inc"
)

if(BUILD_TESTING)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7B42B4D6ED48810C5362C265A17FAEBE90DC2373C885E5216439D37927F02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
    set_target_properties(gtest PROPERTIES FOLDER "Dependencies")
endif()
