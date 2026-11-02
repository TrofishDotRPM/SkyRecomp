// SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104), for co-op's password check: a joining player proves they know the
// password by answering the host's random challenge with an HMAC of it, so the password itself never crosses the network.
// tools/test_coop_sha256.c checks them against the standard test vectors.
#pragma once
#include <stddef.h>
#include <stdint.h>

void coop_sha256(const void *data, size_t n, uint8_t out[32]);
void coop_hmac_sha256(const void *key, size_t key_n, const void *data, size_t n, uint8_t out[32]);
