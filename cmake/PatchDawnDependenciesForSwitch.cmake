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
if(NOT _sysinfo_content MATCHES "PartyBoard libnx: pthread_t is an opaque pointer")
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


set(_cctz_libc "${_absl_root}/absl/time/internal/cctz/src/time_zone_libc.cc")
if(NOT EXISTS "${_cctz_libc}")
    message(FATAL_ERROR "Dawn's Abseil CCTZ libc source was not found")
endif()

file(READ "${_cctz_libc}" _cctz_libc_content)
if(NOT _cctz_libc_content MATCHES "PartyBoard libnx: newlib struct tm")
    # newlib/libnx intentionally lacks the non-standard tm_gmtoff/tm_zone
    # members used by CCTZ on glibc/BSD. Dawn does not require local timezone
    # semantics, so keep this dependency portable by exposing UTC here.
    string(REPLACE
        "#else\n// Adapt to different spellings of the struct std::tm extension fields."
        "#elif defined(__SWITCH__)\n// PartyBoard libnx: newlib struct tm has no gmtoff/zone extensions.\nauto tm_gmtoff(const std::tm&) -> long { return 0; }\nauto tm_zone(const std::tm&) -> const char* { return \"UTC\"; }\n#else\n// Adapt to different spellings of the struct std::tm extension fields."
        _cctz_libc_content "${_cctz_libc_content}")

    if(NOT _cctz_libc_content MATCHES "PartyBoard libnx: newlib struct tm")
        message(FATAL_ERROR "Failed to patch Abseil CCTZ for libnx")
    endif()

    file(WRITE "${_cctz_libc}" "${_cctz_libc_content}")
    message(STATUS "Patched Abseil CCTZ timezone extensions for libnx")
endif()


set(_eglplatform "${DAWN_SOURCE_DIR}/third_party/EGL-Registry/src/api/EGL/eglplatform.h")
if(NOT EXISTS "${_eglplatform}")
    message(FATAL_ERROR "Dawn's EGL-Registry eglplatform.h was not found")
endif()

file(READ "${_eglplatform}" _eglplatform_content)
if(NOT _eglplatform_content MATCHES "defined\\(__SWITCH__\\)")
    # Dawn compiles against the Khronos EGL headers it fetches, which do not
    # know libnx. Use the native types of switch-mesa's own eglplatform.h, the
    # libEGL the probe links: a window is an NWindow*.
    string(REPLACE
        "#else\n#error \"Platform not recognized\""
        "#elif defined(__SWITCH__)\n\ntypedef void            *EGLNativeDisplayType;\ntypedef khronos_uint8_t *EGLNativePixmapType;\ntypedef void            *EGLNativeWindowType;\n\n#else\n#error \"Platform not recognized\""
        _eglplatform_content "${_eglplatform_content}")

    if(NOT _eglplatform_content MATCHES "defined\\(__SWITCH__\\)")
        message(FATAL_ERROR "Failed to add libnx to Dawn's EGL platform types")
    endif()

    file(WRITE "${_eglplatform}" "${_eglplatform_content}")
    message(STATUS "Added libnx native types to Dawn's EGL headers")
endif()


# Abseil's Mutex needs LowLevelAlloc, which Abseil only builds where mmap()
# exists; libnx has none, yet absl::Mutex still references it. Re-enable
# LowLevelAlloc on the Switch with page-aligned heap memory standing in for
# anonymous mappings (no signals either, so no async-signal-safe arena).
# ABSL_HAVE_MMAP stays off, so nothing else starts using mmap/mprotect.
set(_lla_h "${_absl_root}/absl/base/internal/low_level_alloc.h")
set(_lla_cc "${_absl_root}/absl/base/internal/low_level_alloc.cc")
if(NOT EXISTS "${_lla_h}" OR NOT EXISTS "${_lla_cc}")
    message(FATAL_ERROR "Dawn's Abseil LowLevelAlloc sources were not found")
endif()

file(READ "${_lla_h}" _lla_h_content)
if(NOT _lla_h_content MATCHES "defined\\(__SWITCH__\\)")
    string(REPLACE
        "#elif !defined(ABSL_HAVE_MMAP) && !defined(_WIN32)"
        "#elif !defined(ABSL_HAVE_MMAP) && !defined(_WIN32) && !defined(__SWITCH__)"
        _lla_h_content "${_lla_h_content}")
    string(REGEX REPLACE
        "defined\\(__hexagon__\\)(\n#define ABSL_LOW_LEVEL_ALLOC_ASYNC_SIGNAL_SAFE_MISSING 1)"
        "defined(__hexagon__) || defined(__SWITCH__)\\1"
        _lla_h_content "${_lla_h_content}")

    string(REGEX MATCHALL "defined\\(__SWITCH__\\)" _lla_h_hits "${_lla_h_content}")
    list(LENGTH _lla_h_hits _lla_h_hit_count)
    if(NOT _lla_h_hit_count EQUAL 2)
        message(FATAL_ERROR "Failed to enable Abseil LowLevelAlloc for libnx")
    endif()
    file(WRITE "${_lla_h}" "${_lla_h_content}")
endif()

file(READ "${_lla_cc}" _lla_cc_content)
if(NOT _lla_cc_content MATCHES "PartyBoard libnx")
    string(REPLACE
        "#ifndef _WIN32\n#include <pthread.h>\n#include <signal.h>\n#include <sys/mman.h>\n#include <unistd.h>\n#else"
        "#if defined(__SWITCH__)\n// PartyBoard libnx: no mmap(), so arenas take page-aligned, zeroed heap\n// memory instead of anonymous private mappings.\n#include <pthread.h>\n#include <stdlib.h>\n#include <string.h>\n#include <sys/types.h>\n#include <unistd.h>\n#define PROT_READ 0x1\n#define PROT_WRITE 0x2\n#define MAP_PRIVATE 0x2\n#define MAP_ANONYMOUS 0x20\n#define MAP_FAILED (reinterpret_cast<void *>(-1))\nstatic void *mmap(void *, size_t size, int, int, int, off_t) {\n  void *pages = aligned_alloc(0x1000, size);\n  if (pages == nullptr) return MAP_FAILED;\n  memset(pages, 0, size);\n  return pages;\n}\nstatic int munmap(void *pages, size_t) {\n  free(pages);\n  return 0;\n}\n#elif !defined(_WIN32)\n#include <pthread.h>\n#include <signal.h>\n#include <sys/mman.h>\n#include <unistd.h>\n#else"
        _lla_cc_content "${_lla_cc_content}")
    string(REPLACE
        "#elif defined(__wasm__) || defined(__asmjs__) || defined(__hexagon__)\n  return static_cast<size_t>(getpagesize());"
        "#elif defined(__SWITCH__)\n  return 0x1000;\n#elif defined(__wasm__) || defined(__asmjs__) || defined(__hexagon__)\n  return static_cast<size_t>(getpagesize());"
        _lla_cc_content "${_lla_cc_content}")

    string(REGEX MATCHALL "defined\\(__SWITCH__\\)" _lla_cc_hits "${_lla_cc_content}")
    list(LENGTH _lla_cc_hits _lla_cc_hit_count)
    if(NOT _lla_cc_hit_count EQUAL 2)
        message(FATAL_ERROR "Failed to give Abseil LowLevelAlloc heap pages on libnx")
    endif()
    file(WRITE "${_lla_cc}" "${_lla_cc_content}")
    message(STATUS "Enabled Abseil LowLevelAlloc on libnx (heap-backed pages)")
endif()
