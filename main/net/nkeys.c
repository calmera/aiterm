#include "nkeys.h"
#include <string.h>

static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static int b32_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return 26 + (c - '2');
    return -1;
}

int nkeys_base32_decode(const char* s, uint8_t* out, size_t out_len) {
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (; *s && *s != '='; s++) {
        int v = b32_val(*s);
        if (v < 0) return -1;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (n >= out_len) return -1;
            out[n++] = (uint8_t)((acc >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return (int)n;
}

int nkeys_base32_encode(const uint8_t* in, size_t n, char* out, size_t out_len) {
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            if (o + 1 >= out_len) return -1;
            out[o++] = B32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) {
        if (o + 1 >= out_len) return -1;
        out[o++] = B32[(acc << (5 - bits)) & 31];
    }
    out[o] = 0;
    return (int)o;
}

uint16_t nkeys_crc16(const uint8_t* data, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

bool nkeys_seed_decode(const char* seed, uint8_t raw[NKEYS_SEED_RAW], uint8_t* public_prefix) {
    uint8_t buf[64];
    int n = nkeys_base32_decode(seed, buf, sizeof(buf));
    if (n != 2 + NKEYS_SEED_RAW + 2) return false;
    uint16_t crc = (uint16_t)(buf[n - 2] | (buf[n - 1] << 8));
    if (nkeys_crc16(buf, (size_t)n - 2) != crc) return false;
    uint8_t b1 = buf[0], b2 = buf[1];
    if ((b1 & 0xF8) != NKEYS_PREFIX_SEED) return false;
    if (public_prefix) *public_prefix = (uint8_t)(((b1 & 7) << 5) | ((b2 & 0xF8) >> 3));
    memcpy(raw, buf + 2, NKEYS_SEED_RAW);
    return true;
}

bool nkeys_public_encode(uint8_t prefix, const uint8_t key[32], char* out, size_t out_len) {
    uint8_t raw[35];
    raw[0] = prefix;
    memcpy(raw + 1, key, 32);
    uint16_t crc = nkeys_crc16(raw, 33);
    raw[33] = (uint8_t)(crc & 0xFF);
    raw[34] = (uint8_t)(crc >> 8);
    return nkeys_base32_encode(raw, sizeof(raw), out, out_len) == NKEYS_PUBLIC_LEN;
}

#if defined(ESP_PLATFORM)
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ed25519.h>

static bool load_key(const char* seed, ed25519_key* key, uint8_t pub[32]) {
    uint8_t raw[NKEYS_SEED_RAW];
    if (!nkeys_seed_decode(seed, raw, NULL)) return false;
    if (wc_ed25519_init(key) != 0) return false;
    if (wc_ed25519_import_private_only(raw, sizeof(raw), key) != 0) { wc_ed25519_free(key); return false; }
    word32 publen = 32;
    if (wc_ed25519_make_public(key, pub, publen) != 0) { wc_ed25519_free(key); return false; }
    // wolfCrypt signs with a key that holds both halves
    if (wc_ed25519_import_private_key(raw, sizeof(raw), pub, 32, key) != 0) { wc_ed25519_free(key); return false; }
    memset(raw, 0, sizeof(raw));
    return true;
}

bool nkeys_public_from_seed(const char* seed, char* out, size_t out_len) {
    ed25519_key key; uint8_t pub[32]; uint8_t prefix = NKEYS_PREFIX_USER;
    uint8_t raw[NKEYS_SEED_RAW];
    if (!nkeys_seed_decode(seed, raw, &prefix)) return false;
    memset(raw, 0, sizeof(raw));
    if (!load_key(seed, &key, pub)) return false;
    wc_ed25519_free(&key);
    return nkeys_public_encode(prefix, pub, out, out_len);
}

bool nkeys_sign(const char* seed, const uint8_t* nonce, size_t nonce_len, uint8_t sig[64]) {
    ed25519_key key; uint8_t pub[32];
    if (!load_key(seed, &key, pub)) return false;
    word32 siglen = 64;
    int r = wc_ed25519_sign_msg(nonce, (word32)nonce_len, sig, &siglen, &key);
    wc_ed25519_free(&key);
    return r == 0 && siglen == 64;
}
#endif
