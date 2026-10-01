/*
 * Mbed TLS entropy on libnx (MBEDTLS_ENTROPY_HARDWARE_ALT, see
 * mbedtls_switch_config.h): libnx's random generator, seeded by the kernel's
 * entropy source.
 */
#include <stddef.h>

#include <switch/kernel/random.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    randomGet(output, len);
    *olen = len;
    return 0;
}
