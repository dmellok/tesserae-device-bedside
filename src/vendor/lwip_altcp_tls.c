/* lwIP's altcp TLS layer over mbedTLS (HTTPS to GitHub, over-the-air updates),
 * compiled here because the PlatformIO picosdk builder leaves lwIP's apps out.
 * The SDK's patched copy (pico_lwip/altcp_tls_mbedtls.c) is the one that works
 * with mbedTLS 3; scripts/build_mbedtls.py puts its directory and lwIP's
 * altcp_tls directory on the include path. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS            /* the patched layer reads a few internals */
#include "altcp_tls_mbedtls.c"
#include "altcp_tls_mbedtls_mem.c"
