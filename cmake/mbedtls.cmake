# Mbed TLS (Apache-2.0), for the CubeShelf friends the game reads by itself where CubeShelf
# cannot run (src/port/online/cubeshelf_social.cpp): P-256 ECDH, AES-GCM, SHA-256, HMAC and
# PBKDF2. Only the crypto library is built and linked.
#
# The release archive, not a git checkout: it ships the generated sources, so nothing here needs
# Python or the framework submodule. Mbed TLS still declares cmake_minimum_required 3.5.1, which
# leaves option() ignoring plain variables -- hence the cache entries.
set(ENABLE_PROGRAMS OFF CACHE BOOL "Mbed TLS: build programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Mbed TLS: build tests" FORCE)
set(GEN_FILES OFF CACHE BOOL "Mbed TLS: regenerate sources" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Mbed TLS: warnings as errors" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "Mbed TLS: shared libraries" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Mbed TLS: static libraries" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "Mbed TLS: install rules" FORCE)

FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(mbedtls)

# Linked into the dol shared library.
set_target_properties(mbedcrypto PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "extern")
