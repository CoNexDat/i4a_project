idf_component_register(
    SRCS "src/channel_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES node ring_share neighbor_manager esp_timer os
)
