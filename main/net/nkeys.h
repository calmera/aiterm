// NKeys helpers for the chronicle identity bridge: decode the sentinel's seed, derive its public
// key and sign the server nonce (ed25519 through the vendored wolfCrypt). The base32/CRC parts are
// plain C and host-tested; signing is target only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NKEYS_SEED_RAW   32
#define NKEYS_PUBLIC_LEN 56   // base32 of 1 prefix + 32 key + 2 crc bytes
#define NKEYS_PREFIX_SEED 144 // 18 << 3
#define NKEYS_PREFIX_USER 160 // 20 << 3

// RFC 4648 base32 without padding. Returns bytes written, or -1 on bad input / short buffer.
int      nkeys_base32_decode(const char* s, uint8_t* out, size_t out_len);
int      nkeys_base32_encode(const uint8_t* in, size_t n, char* out, size_t out_len);
uint16_t nkeys_crc16(const uint8_t* data, size_t n);  // CRC-16/XMODEM as nkeys uses it

// Decodes an "S..." seed: checks the CRC and the seed prefix, returns the 32 raw bytes and the
// public-key prefix byte the seed was made for (NKEYS_PREFIX_USER for a user seed).
bool nkeys_seed_decode(const char* seed, uint8_t raw[NKEYS_SEED_RAW], uint8_t* public_prefix);
// Encodes a public key as "U..." (or another prefix): 56 chars + NUL.
bool nkeys_public_encode(uint8_t prefix, const uint8_t key[32], char* out, size_t out_len);

#if defined(ESP_PLATFORM)
// Derives the public key string from a seed and signs a nonce (64-byte signature). Target only.
bool nkeys_public_from_seed(const char* seed, char* out, size_t out_len);
bool nkeys_sign(const char* seed, const uint8_t* nonce, size_t nonce_len, uint8_t sig[64]);
#endif

#ifdef __cplusplus
}
#endif
