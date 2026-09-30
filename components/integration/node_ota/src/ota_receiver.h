#pragma once

#include "node_ota/protocol.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#define NODE_OTA_IDLE_TIMEOUT_US (120LL * 1000000)

typedef struct {
    uint8_t role, phase, targets;
    uint32_t session, size, received;
    int64_t last_activity;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    bool handle_open;
    bool boot_recovery_pending;
    mbedtls_sha256_context hash;
    uint8_t expected_hash[32];
    bool cached;
    node_ota_packet_t previous, response;
} ota_receiver_t;

void ota_receiver_init(ota_receiver_t *receiver, uint8_t role);
void ota_receiver_handle(ota_receiver_t *receiver, const node_ota_packet_t *request,
                         node_ota_packet_t *reply);
void ota_receiver_expire(ota_receiver_t *receiver);
