include_guard(GLOBAL)

option(TSM_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(TSM_INSTALL_ASSETS "Include local media assets when installing or packaging" ON)

function(tsm_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            UNICODE
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
            _UNICODE
            _WIN32_WINNT=0x0A00
        )
        target_compile_options(${target} PRIVATE
            /MP
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
        if(TSM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
        if(TSM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
