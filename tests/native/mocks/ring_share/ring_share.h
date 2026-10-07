#ifndef NATIVE_MOCK_RING_SHARE_H
#define NATIVE_MOCK_RING_SHARE_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { int unused; } ring_share_t;
typedef struct {
    void (*callback)(void *, const uint8_t *, uint16_t);
    void *context;
} ring_callback_t;
#define RS_NODE_OTA 7
#define RS_CHANNEL_MANAGER 3
#define RS_NEIGHBOR_MANAGER 8
#define RS_MAX_BROADCAST_LEN 512
void rs_register_component(ring_share_t *, int, ring_callback_t);
bool rs_broadcast(ring_share_t *, int, const void *, uint16_t);
#endif
