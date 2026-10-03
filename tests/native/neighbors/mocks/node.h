#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t node_device_orientation_t;
uint8_t node_get_device_orientation(void);
const char *node_get_uuid(void);
bool node_is_network_provided(void);
void node_disable_sta(void);
