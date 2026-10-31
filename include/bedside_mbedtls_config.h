/*
 * bedside_mbedtls_config.h: (not mbedtls_config.h: mbedTLS would find its own default next to build_info.h) a small TLS 1.2 client for HTTPS to GitHub (over-the-air
 * updates). ECDHE with ECDSA or RSA, AES-GCM, certificates checked against
 * src/tls_roots.c. Memory comes from the PSRAM heap (psram_heap.c), not lwIP's.
 * No clock is assumed, so certificate dates are not checked.
 */
#pragma once
#include <stddef.h>

void *psram_heap_calloc(size_t n, size_t size);
void  psram_heap_free(void *p);
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO psram_heap_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO   psram_heap_free

#define MBEDTLS_ENTROPY_HARDWARE_ALT          /* mbedtls_hardware_poll: the RP2350's RNG */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES                /* tables in flash, not RAM */
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#define MBEDTLS_HAVE_TIME                     /* session timestamps (dates on certificates stay unchecked) */
#define MBEDTLS_PLATFORM_MS_TIME_ALT          /* mbedtls_ms_time: the board timer (net_rest.c) */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384          /* servers send full-size records */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
