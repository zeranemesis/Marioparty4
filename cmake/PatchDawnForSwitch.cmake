if(NOT DEFINED DAWN_SOURCE_DIR)
    message(FATAL_ERROR "DAWN_SOURCE_DIR is required")
endif()

set(_platform "${DAWN_SOURCE_DIR}/src/utils/platform.h")
if(NOT EXISTS "${_platform}")
    message(FATAL_ERROR "Dawn platform header not found: ${_platform}")
endif()

file(READ "${_platform}" _content)

if(_content MATCHES "DAWN_PLATFORM_IS_SWITCH")
    message(STATUS "Dawn Switch platform patch already applied")
    return()
endif()

set(_anchor_os "#elif defined(__Fuchsia__)\n#define DAWN_PLATFORM_IS_FUCHSIA 1\n#define DAWN_PLATFORM_IS_POSIX 1")
set(_switch_os "#elif defined(__SWITCH__)\n#define DAWN_PLATFORM_IS_SWITCH 1\n#define DAWN_PLATFORM_IS_POSIX 1\n\n#elif defined(__Fuchsia__)\n#define DAWN_PLATFORM_IS_FUCHSIA 1\n#define DAWN_PLATFORM_IS_POSIX 1")
string(REPLACE "${_anchor_os}" "${_switch_os}" _content "${_content}")

set(_anchor_default "#if !defined(DAWN_PLATFORM_IS_FUCHSIA)\n#define DAWN_PLATFORM_IS_FUCHSIA 0\n#endif")
set(_switch_default "#if !defined(DAWN_PLATFORM_IS_SWITCH)\n#define DAWN_PLATFORM_IS_SWITCH 0\n#endif\n\n#if !defined(DAWN_PLATFORM_IS_FUCHSIA)\n#define DAWN_PLATFORM_IS_FUCHSIA 0\n#endif")
string(REPLACE "${_anchor_default}" "${_switch_default}" _content "${_content}")

if(NOT _content MATCHES "defined\\(__SWITCH__\\)")
    message(FATAL_ERROR "Failed to patch Dawn OS detection for __SWITCH__")
endif()

file(WRITE "${_platform}" "${_content}")
message(STATUS "Patched Dawn platform detection for libnx")
