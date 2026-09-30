#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct { int unused; } ring_share_t;
typedef struct {
    void (*callback)(void *, const uint8_t *, uint16_t);
    void *context;
} ring_callback_t;
#define RS_NODE_OTA 7
void rs_register_component(ring_share_t *, int, ring_callback_t);
bool rs_broadcast(ring_share_t *, int, const void *, uint16_t);
