#pragma once

#include "esp_err.h"
#include "ring_share/ring_share.h"
#include "node_ota/protocol.h"

/* Register before starting the ring. Call on every board, using its GPIO role. */
esp_err_t node_ota_init(ring_share_t *ring, uint8_t role);
/* Only the center opens UART0; call once node_setup has finished. */
esp_err_t node_ota_start_serial(void);

typedef void (*node_ota_reply_fn)(const node_ota_packet_t *reply, void *context);
/* Transport-neutral coordinator entry point. Serializes callers, waits for all
 * requested boards, and invokes reply() for each ACK. Future authenticated
 * network ingress can call this from its own task. REBOOT requires the caller
 * to flush its response and call esp_restart() ONLY when this returns ESP_OK. */
esp_err_t node_ota_exchange(const node_ota_packet_t *request,
                            node_ota_reply_fn reply, void *context);

/* UART diagnostic independent of CONFIG_LOG_DEFAULT_LEVEL; does not alter OTA state. */
void node_ota_boot_trace(const char *stage);
/* Pending OTA boots must confirm within 90 seconds. After successful confirmation,
 * restart locally once at ~120 seconds to recover the ring. Call before node_setup. */
void node_ota_watch_boot(void);
esp_err_t node_ota_confirm_boot(void);
