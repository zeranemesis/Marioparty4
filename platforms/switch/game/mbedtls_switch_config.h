/*
 * Mbed TLS on libnx (MBEDTLS_USER_CONFIG_FILE, applied on top of the default
 * configuration). PartyBoard only builds the crypto library, for the
 * CubeShelf friends code (cmake/mbedtls.cmake).
 *
 * Mbed TLS knows Unix and Windows for its clock, timer and entropy; newlib
 * on the Switch is neither. Nothing the port uses needs the clock or the
 * timer, and entropy comes from libnx (mbedtls_switch_entropy.c).
 */
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_HAVE_TIME

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
