idf_component_register(
    SRCS "src/registry.c" "src/neighbor_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES ring_share os esp_timer
)
