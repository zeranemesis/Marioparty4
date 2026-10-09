# Tracy's TracySystem.cpp is compiled even with TRACY_ENABLE off (Aurora
# names threads through it) and stops on platforms it does not know. On
# libnx: no kernel thread id to report (as on Emscripten) and no login name.
# Applied to FetchContent's copy only, like the Dawn patches.

if(NOT DEFINED TRACY_SOURCE_DIR)
    message(FATAL_ERROR "TRACY_SOURCE_DIR is required")
endif()

set(_tracy_system "${TRACY_SOURCE_DIR}/public/common/TracySystem.cpp")
if(NOT EXISTS "${_tracy_system}")
    message(FATAL_ERROR "Tracy's TracySystem.cpp was not found: ${_tracy_system}")
endif()

file(READ "${_tracy_system}" _content)
if(NOT _content MATCHES "__SWITCH__")
    string(REPLACE
        "#elif defined __EMSCRIPTEN__\n    // Not supported, but let it compile.\n    return 0;"
        "#elif defined __EMSCRIPTEN__ || defined __SWITCH__\n    // Not supported, but let it compile.\n    return 0;"
        _content "${_content}")
    string(REPLACE
        "#elif defined __ANDROID__\n    const auto user = getlogin();"
        "#elif defined __SWITCH__\n    return \"(?)\";\n#elif defined __ANDROID__\n    const auto user = getlogin();"
        _content "${_content}")

    string(REGEX MATCHALL "__SWITCH__" _hits "${_content}")
    list(LENGTH _hits _hit_count)
    if(NOT _hit_count EQUAL 2)
        message(FATAL_ERROR "Failed to patch Tracy for libnx")
    endif()
    file(WRITE "${_tracy_system}" "${_content}")
    message(STATUS "Patched Tracy for libnx")
endif()
