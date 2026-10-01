if(NOT DEFINED DAWN_SOURCE_DIR)
    message(FATAL_ERROR "DAWN_SOURCE_DIR is required")
endif()

# Dawn fetches Abseil during its own configure step. This script therefore runs
# after FetchContent_MakeAvailable(dawn), but before Ninja compiles anything.
# Only the build-tree copy is modified.

set(_absl_root "${DAWN_SOURCE_DIR}/third_party/abseil-cpp")
set(_sysinfo "${_absl_root}/absl/base/internal/sysinfo.cc")
set(_thread_identity "${_absl_root}/absl/base/internal/thread_identity.cc")

if(NOT EXISTS "${_sysinfo}" OR NOT EXISTS "${_thread_identity}")
    message(FATAL_ERROR
        "Dawn's fetched Abseil sources were not found under ${_absl_root}")
endif()

file(READ "${_sysinfo}" _sysinfo_content)
if(NOT _sysinfo_content MATCHES "PartyBoard libnx: pthread_t is a pointer")
    string(REPLACE
        "return static_cast<pid_t>(pthread_self());"
        "// PartyBoard libnx: pthread_t is an opaque pointer, not an arithmetic type.\n  // Fold the pointer value into pid_t; Abseil only needs a stable per-thread key here.\n  return static_cast<pid_t>(reinterpret_cast<intptr_t>(pthread_self()));"
        _sysinfo_content "${_sysinfo_content}")

    if(NOT _sysinfo_content MATCHES "reinterpret_cast<intptr_t>\\(pthread_self\\(\\)\\)")
        message(FATAL_ERROR "Failed to patch Abseil GetTID for libnx")
    endif()

    file(WRITE "${_sysinfo}" "${_sysinfo_content}")
    message(STATUS "Patched Abseil GetTID for libnx pthread_t")
endif()

file(READ "${_thread_identity}" _thread_identity_content)
if(NOT _thread_identity_content MATCHES "defined\\(__SWITCH__\\)")
    # libnx pthreads support pthread_setspecific, but newlib/libnx does not
    # expose the POSIX signal-mask API used by Abseil's glibc workaround.
    string(REPLACE
        "defined(__hexagon__)"
        "defined(__hexagon__) || defined(__SWITCH__)"
        _thread_identity_content "${_thread_identity_content}")

    if(NOT _thread_identity_content MATCHES "defined\\(__SWITCH__\\)")
        message(FATAL_ERROR "Failed to patch Abseil thread identity for libnx")
    endif()

    file(WRITE "${_thread_identity}" "${_thread_identity_content}")
    message(STATUS "Patched Abseil thread identity signal handling for libnx")
endif()


set(_elf_mem_image "${_absl_root}/absl/debugging/internal/elf_mem_image.h")
if(NOT EXISTS "${_elf_mem_image}")
    message(FATAL_ERROR "Dawn's Abseil elf_mem_image.h was not found")
endif()

file(READ "${_elf_mem_image}" _elf_mem_image_content)
if(NOT _elf_mem_image_content MATCHES "!defined\\(__SWITCH__\\)")
    # libnx/newlib produces ELF binaries but does not provide the glibc-style
    # <link.h> runtime loader interfaces used by Abseil's in-memory symbol
    # image support. Disable that optional debugging facility on Switch.
    string(REPLACE
        "!defined(__XTENSA__)"
        "!defined(__XTENSA__) && !defined(__SWITCH__)"
        _elf_mem_image_content "${_elf_mem_image_content}")

    if(NOT _elf_mem_image_content MATCHES "!defined\\(__SWITCH__\\)")
        message(FATAL_ERROR "Failed to disable Abseil ELF memory image on libnx")
    endif()

    file(WRITE "${_elf_mem_image}" "${_elf_mem_image_content}")
    message(STATUS "Disabled Abseil ELF memory image support on libnx")
endif()
