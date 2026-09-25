# Mbed TLS (Apache-2.0), for the CubeShelf friends the game reads by itself where CubeShelf
# cannot run (src/port/online/cubeshelf_social.cpp): P-256 ECDH, AES-GCM, SHA-256, HMAC and
# PBKDF2. Only the crypto library is built, with its default configuration.
#
# Downloaded only, never entered: SOURCE_SUBDIR names a directory without a CMakeLists.txt, so
# FetchContent does not add Mbed TLS's own project. That project calls project(), find_package()
# for Threads and Python and declares a dozen generic cache options (ENABLE_TESTING, GEN_FILES,
# ...), all of which then land in this build's cache before Aurora and Dawn configure; on the
# macOS x86_64 build, which configures Dawn and Abseil from source, Abseil's C++17 check then
# failed. The sources below are library/CMakeLists.txt's src_crypto for this exact version, and
# the release archive already carries the generated files they need.
FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR partyboard-download-only
)
FetchContent_MakeAvailable(mbedtls)

set(MBEDTLS_CRYPTO_SOURCES
        aes.c aesni.c aesce.c aria.c asn1parse.c asn1write.c base64.c bignum.c bignum_core.c
        bignum_mod.c bignum_mod_raw.c block_cipher.c camellia.c ccm.c chacha20.c chachapoly.c
        cipher.c cipher_wrap.c constant_time.c cmac.c ctr_drbg.c des.c dhm.c ecdh.c ecdsa.c
        ecjpake.c ecp.c ecp_curves.c entropy.c entropy_poll.c error.c gcm.c hkdf.c hmac_drbg.c
        lmots.c lms.c md.c md5.c memory_buffer_alloc.c nist_kw.c oid.c padlock.c pem.c pk.c
        pk_ecc.c pk_wrap.c pkcs12.c pkcs5.c pkparse.c pkwrite.c platform.c platform_util.c
        poly1305.c psa_crypto.c psa_crypto_aead.c psa_crypto_cipher.c psa_crypto_client.c
        psa_crypto_driver_wrappers_no_static.c psa_crypto_ecp.c psa_crypto_ffdh.c psa_crypto_hash.c
        psa_crypto_mac.c psa_crypto_pake.c psa_crypto_rsa.c psa_crypto_random.c psa_crypto_se.c
        psa_crypto_slot_management.c psa_crypto_storage.c psa_its_file.c psa_util.c ripemd160.c
        rsa.c rsa_alt_helpers.c sha1.c sha256.c sha512.c sha3.c threading.c timing.c version.c
        version_features.c
)
list(TRANSFORM MBEDTLS_CRYPTO_SOURCES PREPEND ${mbedtls_SOURCE_DIR}/library/)

add_library(mbedcrypto STATIC ${MBEDTLS_CRYPTO_SOURCES})
target_include_directories(mbedcrypto PUBLIC ${mbedtls_SOURCE_DIR}/include PRIVATE ${mbedtls_SOURCE_DIR}/library)
# Linked into the dol shared library.
set_target_properties(mbedcrypto PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99 FOLDER "extern")
if (WIN32)
    # The Windows entropy source is BCryptGenRandom.
    target_link_libraries(mbedcrypto PUBLIC bcrypt)
endif ()
if (MSVC)
    target_compile_options(mbedcrypto PRIVATE /W0)
else ()
    target_compile_options(mbedcrypto PRIVATE -w)
endif ()
