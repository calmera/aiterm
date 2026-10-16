# wolfSSH 1.4.20 (GPLv3 / commercial) as an IDF component; client-only, custom I/O, terminal.
# Vendored from kaistierl/why2025-badge-firmware sdk_apps/sshterm/thirdparty/wolfssh-1.4.20.
set(WOLFSSH_SRCS "")
if(CONFIG_AITERM_SSH)
    file(GLOB WOLFSSH_SRCS "src/*.c")
    list(FILTER WOLFSSH_SRCS EXCLUDE REGEX ".*/(agent|certman|wolfsftp|wolfscp|keygen|misc)\\.c$")
endif()
idf_component_register(
    SRCS ${WOLFSSH_SRCS}
    INCLUDE_DIRS "."
    REQUIRES wolfssl
    PRIV_REQUIRES lwip
)
if(CONFIG_AITERM_SSH)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC WOLFSSL_USER_SETTINGS WOLFSSH_USER_SETTINGS)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-sign-compare -Wno-implicit-fallthrough -Wno-format -Wno-maybe-uninitialized)
endif()
