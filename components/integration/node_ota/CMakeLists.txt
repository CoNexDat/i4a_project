idf_component_register(
    SRCS "src/node_ota.c" "src/ota_receiver.c" "src/ota_protocol.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ring_share app_update esp_app_format esp_timer driver mbedtls
    PRIV_REQUIRES reset_manager
)
