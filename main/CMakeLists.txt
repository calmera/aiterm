file(GLOB_RECURSE AITERM_SRCS
    "*.c" "*.cpp")
list(FILTER AITERM_SRCS EXCLUDE REGEX ".*/fonts/.*\\.c$")
file(GLOB AITERM_FONT_SRCS "fonts/*.c")

idf_component_register(
    SRCS ${AITERM_SRCS} ${AITERM_FONT_SRCS}
    INCLUDE_DIRS "." "util"
    PRIV_REQUIRES
        esp_lcd
        nvs_flash
        spiffs
        bt
        esp_wifi
        esp_netif
        esp_event
        esp-tls
        esp_http_client
        esp_http_server
        esp_timer
        esp_psram
        esp_mm
        esp_hw_support
        esp_driver_gpio
        mbedtls
        lwip
        badge-bsp
        tanmatsu-wifi
        wifi-manager
        esp-hosted-tanmatsu
        esp-wifi-remote-tanmatsu
        lvgl
        cjson
        qrcodegen
        esp_ringbuf
        wolfssl
        wolfssh
        espidf-nats
    EMBED_FILES
        "provisioning/index.html"
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-function" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)
