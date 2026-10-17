# espidf-nats 1.4.0 (debsahu/espidf-nats @ 73b7118), vendored. Header-only C++ NATS client with
# JetStream, used by main/net/nats_glue.cpp. Websocket transport is not built (ESP-IDF 6 has no
# built-in esp_websocket_client and the hosted chronicle exposes plain TLS on 4222). The cJSON
# dependency is the project's vendored `cjson` component.
idf_component_register(
    INCLUDE_DIRS include
    REQUIRES esp-tls lwip mbedtls cjson
)
