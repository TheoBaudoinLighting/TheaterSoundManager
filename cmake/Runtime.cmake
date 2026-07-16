include_guard(GLOBAL)

function(tsm_stage_runtime target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "Unknown runtime target: ${target}")
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>"
            "$<TARGET_FILE:FMOD::FMOD>"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Staging SDL2 and FMOD beside ${target}"
        VERBATIM
    )
endfunction()
