# wolfSSL 5.8.2 (wolfCrypt only, GPLv3 / commercial — see LICENSING) as an IDF component.
# Vendored from kaistierl/why2025-badge-firmware sdk_apps/sshterm/thirdparty/wolfssl-5.8.2.
set(WOLF_SRCS "")
if(CONFIG_AITERM_SSH)
    file(GLOB WOLF_SRCS "wolfcrypt/src/*.c")
    # files that are not meant to be compiled standalone / need other backends
    list(FILTER WOLF_SRCS EXCLUDE REGEX ".*/(evp|aes_asm|aes_gcm_asm|aes_xts_asm|sha256_asm|sha512_asm|sha3_asm|chacha_asm|poly1305_asm|sp_x86_64|sp_arm32|sp_arm64|sp_armthumb|sp_cortexm|sp_c32|sp_c64|sp_dsp32|sp_int|sp_sm2_x86_64|sp_sm2_arm32|sp_sm2_arm64|sp_sm2_armthumb|sp_sm2_cortexm|sp_sm2_c32|sp_sm2_c64|fe_x25519_asm|fe_x25519_128|fe_x25519_asm|ge_x25519_asm|ed25519_asm|fe_448|ge_448|ed448|curve448|async|selftest|fips|fips_test|wolfcrypt_first|wolfcrypt_last|cryptocb|cmac|camellia|arc4|md2|md4|rc2|ripemd|blake2b|blake2s|dsa|srp|pkcs7|pkcs12|siphash|sm2|sm3|sm4|hpke|eccsi|sakke|lms|xmss|ext_lms|ext_xmss|ext_kyber|ext_mlkem|wc_lms|wc_lms_impl|wc_xmss|wc_xmss_impl|wc_mlkem|wc_mlkem_poly|wc_kyber|wc_kyber_poly|dilithium|falcon|sphincs|chacha20_poly1305|chacha|poly1305|wc_dsp|wc_pkcs11|wc_encrypt|rsa_hw|cpuid|wolfevent|wolfmath_stub)\\.c$")
    list(APPEND WOLF_SRCS "port/esp_seed.c")
endif()
idf_component_register(
    SRCS ${WOLF_SRCS}
    INCLUDE_DIRS "." "port"
    PRIV_REQUIRES esp_hw_support
)
if(CONFIG_AITERM_SSH)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC WOLFSSL_USER_SETTINGS)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-sign-compare -Wno-implicit-fallthrough -Wno-cpp -Wno-maybe-uninitialized -Wno-char-subscripts -Wno-type-limits -Wno-format)
endif()
