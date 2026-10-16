/* user_settings.h — wolfSSL/wolfSSH configuration for ai-terminal (ESP-IDF, ESP32-P4).
 * Adapted from kaistierl/why2025-badge-firmware sdk_apps/sshterm (GPLv3 wolfSSL licence).
 * SSH client only: wolfCrypt + wolfSSH, no TLS, software crypto, custom socket I/O. */
#ifndef AITERM_USER_SETTINGS_H
#define AITERM_USER_SETTINGS_H

#define WOLFCRYPT_ONLY
#define NO_TLS
#define WOLFSSL_WOLFSSH

/* wolfSSL is only ever used from the single SSH task. */
#define SINGLE_THREADED
#define NO_WOLFSSL_MULTITHREADING
#define WOLFSSL_NO_MULTITHREAD

#define WOLFSSL_USER_IO
#define WOLFSSH_USER_IO
#define WOLFSSH_TERM
#define NO_WOLFSSL_SERVER
#define NO_SESSION_CACHE

#define WOLFSSL_SMALL_STACK
#define WC_NO_HARDEN
#define NO_WRITEV
#define NO_DEV_RANDOM
#define NO_DEV_URANDOM
#define BENCH_EMBEDDED
#define NO_TERMIOS
#define NO_MAIN_DRIVER
#define NO_INLINE
/* NO_FILESYSTEM must stay unset: wolfSSH_ChangeTerminalSize is compiled out otherwise */

/* RNG seed from the ESP32-P4 hardware RNG (components/wolfssl/port/esp_seed.c) */
extern int aiterm_wolf_generate_seed(unsigned char* output, unsigned int sz);
#define CUSTOM_RAND_GENERATE_SEED aiterm_wolf_generate_seed

#define HAVE_ECC
#define HAVE_RSA
#define HAVE_DH
#define HAVE_SHA256
#define HAVE_HMAC
#define HAVE_AES
#define HAVE_AESGCM
#define HAVE_AES_CBC
#define WOLFSSL_AES_DIRECT
#define WOLFSSL_AES_COUNTER
#define HAVE_FFDHE_2048
#define HAVE_FFDHE_3072
#define HAVE_FFDHE_4096
#define WOLFSSL_SHA224
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define HAVE_CURVE25519
#define CURVE25519_SMALL
#define HAVE_ED25519
#define HAVE_ECC_NISTP256
#define HAVE_ECC_NISTP384
#define HAVE_ECC_NISTP521
#define OPENSSL_EXTRA

#define NO_DSA
#define NO_PSK
#define NO_MD4
#define NO_RC4
#define NO_OLD_TLS

#define RSA_LOW_MEM
#define FP_MAX_BITS 8192
#define RSA_MAX_SIZE 4096
#define USE_FAST_MATH
#define TFM_TIMING_RESISTANT
#define ECC_TIMING_RESISTANT
#define WC_RSA_BLINDING

#define WOLFSSL_KEY_GEN
#define WOLFSSL_ASN_TEMPLATE

/* software crypto only (no ESP32-P4 accelerator glue in this port) */
#define NO_ESP32_CRYPT
#define NO_WOLFSSL_ESP32_CRYPT_HASH
#define NO_WOLFSSL_ESP32_CRYPT_AES
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI

/* time()/gmtime come from newlib; sockets from lwIP via our own I/O callbacks */
#define HAVE_GMTIME_R
#define WOLFSSL_IGNORE_FILE_WARN

#endif
