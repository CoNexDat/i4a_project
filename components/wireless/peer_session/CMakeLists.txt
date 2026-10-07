idf_component_register(
    SRCS "peer_session.c"
    INCLUDE_DIRS "include"
    REQUIRES neighbor_manager node os esp_timer esp_hw_support lwip
)

# Existing node -> device -> server/client cycles now include the admission layer.
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY LINK_INTERFACE_MULTIPLICITY 5)
