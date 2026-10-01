if(NOT DEFINED DAWN_SOURCE_DIR)
    message(FATAL_ERROR "DAWN_SOURCE_DIR is required")
endif()

# This patch is deliberately applied only to FetchContent's build-tree copy of
# Dawn. The pinned upstream source and PartyBoard's Aurora submodule stay clean.

set(_platform "${DAWN_SOURCE_DIR}/src/utils/platform.h")
if(NOT EXISTS "${_platform}")
    message(FATAL_ERROR "Dawn platform header not found: ${_platform}")
endif()

file(READ "${_platform}" _platform_content)
if(NOT _platform_content MATCHES "DAWN_PLATFORM_IS_SWITCH")
    set(_anchor_os "#elif defined(__Fuchsia__)\n#define DAWN_PLATFORM_IS_FUCHSIA 1\n#define DAWN_PLATFORM_IS_POSIX 1")
    set(_switch_os "#elif defined(__SWITCH__)\n#define DAWN_PLATFORM_IS_SWITCH 1\n#define DAWN_PLATFORM_IS_POSIX 1\n\n#elif defined(__Fuchsia__)\n#define DAWN_PLATFORM_IS_FUCHSIA 1\n#define DAWN_PLATFORM_IS_POSIX 1")
    string(REPLACE "${_anchor_os}" "${_switch_os}" _platform_content "${_platform_content}")

    set(_anchor_default "#if !defined(DAWN_PLATFORM_IS_FUCHSIA)\n#define DAWN_PLATFORM_IS_FUCHSIA 0\n#endif")
    set(_switch_default "#if !defined(DAWN_PLATFORM_IS_SWITCH)\n#define DAWN_PLATFORM_IS_SWITCH 0\n#endif\n\n#if !defined(DAWN_PLATFORM_IS_FUCHSIA)\n#define DAWN_PLATFORM_IS_FUCHSIA 0\n#endif")
    string(REPLACE "${_anchor_default}" "${_switch_default}" _platform_content "${_platform_content}")

    if(NOT _platform_content MATCHES "defined\\(__SWITCH__\\)")
        message(FATAL_ERROR "Failed to patch Dawn OS detection for __SWITCH__")
    endif()

    file(WRITE "${_platform}" "${_platform_content}")
    message(STATUS "Patched Dawn platform detection for libnx")
endif()

# The OpenGLES adapter is supplied an existing EGLDisplay + eglGetProcAddress,
# so Dawn never needs to dynamically load libEGL on Switch. libnx has no
# dlfcn.h; make DynamicLib an explicit unsupported/no-op service on this target
# instead of pretending the console is Linux.
set(_dynamic "${DAWN_SOURCE_DIR}/src/dawn/common/DynamicLib.cpp")
if(NOT EXISTS "${_dynamic}")
    message(FATAL_ERROR "Dawn DynamicLib source not found: ${_dynamic}")
endif()

file(READ "${_dynamic}" _dynamic_content)
if(NOT _dynamic_content MATCHES "Dynamic loading is unavailable on libnx")
    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n#include <dlfcn.h>"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n// Dynamic loading is unavailable on libnx. The Switch OpenGLES path supplies\n// eglGetProcAddress and EGLDisplay explicitly, so no shared library is needed.\n#elif DAWN_PLATFORM_IS(POSIX)\n#include <dlfcn.h>"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW);"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n    mHandle = nullptr;\n    if (error != nullptr) {\n        *error = \"Dynamic loading is unavailable on libnx\";\n    }\n#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW);"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW | RTLD_NOLOAD);"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n    mHandle = nullptr;\n    if (error != nullptr) {\n        *error = \"Dynamic loading is unavailable on libnx\";\n    }\n#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW | RTLD_NOLOAD);"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n        dlclose(mHandle);"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n        // No dynamic libraries are opened on libnx.\n#elif DAWN_PLATFORM_IS(POSIX)\n        dlclose(mHandle);"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n    proc = reinterpret_cast<void*>(dlsym(mHandle, procName.c_str()));"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n    if (error != nullptr) {\n        *error = \"Dynamic loading is unavailable on libnx\";\n    }\n#elif DAWN_PLATFORM_IS(POSIX)\n    proc = reinterpret_cast<void*>(dlsym(mHandle, procName.c_str()));"
        _dynamic_content "${_dynamic_content}")

    if(NOT _dynamic_content MATCHES "DAWN_PLATFORM_IS\\(SWITCH\\)")
        message(FATAL_ERROR "Failed to patch Dawn DynamicLib for libnx")
    endif()

    file(WRITE "${_dynamic}" "${_dynamic_content}")
    message(STATUS "Patched Dawn DynamicLib for external EGL/libnx")
endif()
